#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>

#include "linkserver.h"
#include "linkstream.h"
#include "muxserver.h"
#include "muxstream.h"
#include "netlink.h"
#include "socketobject.h"
#include "tcpmux.h"

/* Switches used to disable parts of tcpmux.
 *
 * NO_ZIP - Disable usage of zlib library to compress data.
 * NO_SSL - Disable usage of SSL to encrypt data.
 */

// TODO: Use unsigned computation where appropriate.

#ifndef NO_ZIP
#include "zlib.h"
#define Z_LEVEL Z_DEFAULT_COMPRESSION
#endif

#define CLOG(s) //puts(s)
#define VLOG(s) //puts(s)
#define VVLOG(s) //puts(s)

/* Indicates how long it takes for a broken connection on the demux side to have
 * its associated state erased. */
#define MUX_TIMEOUT 30*60*1000 // In ms
#define RTT_INFINITE 10*1000*1000 // In us
#define MAX_EVENTS 128

static int muxloop(int sserv, int demux, struct addrinfo* caddrinfo);

bool socket_connected(int s) {
  assert(s != -1);

  int err;
  socklen_t errlen = sizeof(err);
  if(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen)) {
    perror("getsockopt");
    return false;
  }
  return err == 0;
}

bool setnonblocking(int s) {
  assert(s != -1);

  int flags;
  flags = fcntl(s, F_GETFL, 0);
  if(flags == -1 ||
     fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl");
    return false;
  }
  return true;
}

int initiate_connect(struct addrinfo* caddrinfo) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  setnonblocking(s);

  int res;
  while((res = connect(s, caddrinfo->ai_addr, caddrinfo->ai_addrlen))) {
    if(errno == EINPROGRESS) {
      break;
    } else if(errno == ENETUNREACH) {
      struct timespec ts;
      ts.tv_sec = 8;
      ts.tv_nsec = 0;
      nanosleep(&ts, NULL);
    } else {
      perror("connect");
      return -1;
    }
  }

  return s;
}

void MuxContext::add_descriptor(EventObject* obj, int fd, int opts) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = (opts & READ ? EPOLLIN : 0) |
              (opts & WRITE ? EPOLLOUT : 0);
  ev.data.ptr = obj;
  if(epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev)) {
    perror("epoll_ctl add_descriptor");
  }
}

void MuxContext::mod_descriptor(EventObject* obj, int fd, int opts) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = (opts & READ ? EPOLLIN : 0) |
              (opts & WRITE ? EPOLLOUT : 0);
  ev.data.ptr = obj;
  if(epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &ev)) {
    perror("epoll_ctl mod_descriptor");
  }
}

void MuxContext::del_descriptor(EventObject* obj, int fd) {
  if(epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, (struct epoll_event*)-1)) {
    perror("epoll_ctl del_descriptor");
  }
  if(obj) {
    delete_list.push_back(obj);
  }
}

int main(int argc, char** argv) {
  srand(time(NULL));
  signal(SIGPIPE, SIG_IGN);

  char* str;
  int demux = 0;
  int retry_dns = 0;
  int daemon = 0;
  if(*argv) for(++argv, --argc; *argv && (*argv)[0] == '-'; ++argv, --argc) {
    if(!strcmp("--demux", *argv)) {
      demux = 1;
    } else if(!strcmp("--retry", *argv)) {
      retry_dns = 1;
    } else if(!strcmp("--daemon", *argv)) {
      daemon = 1;
    }
  }

  if(argc != 2) {
    printf(
"tcpmux is a tool built to multiplex several tcp connections over a single\n"
"connection.  tcpmux expects that it has an instance running with the same\n"
"options (unless otherwise stated) with the server given the --demux switch.\n"
"By default tcpmux will bind to the loopback iterface in 'mux' mode and\n"
"the wildcard address (INADDR_ANY) in 'demux' mode.  You can use * as the\n"
"bind_addr to force the wildcard address to be used.\n\n");
    printf("tcpmux [options] "
           "[bind_addr:]bind_port connect_addr:connect_port\n\n");
    printf("  [optons]\n");
    // TODO: Make optional?
    //printf("  --recon   : Attempt to reconnect muxes when connection lost\n");
    printf("  --demux   : Demultiplex mode (rather than multiplex)\n");
    //printf("  --zip     : Compress stream with zlib\n");
    //printf("  --ssl     : Encrypt stream with SSL\n");
    printf("  --retry   : "
                  "Keep trying to resolve connect host until it suceeds\n");
    // TODO: Add max client switch
    // TODO: Need to provide keys?
    return 0;
  }

  char* baddr = NULL;
  char* bport = argv[0];
  char* caddr = NULL;
  char* cport = argv[1];
  for(str = argv[0]; *str; ++str) {
    if(*str == ':') {
      *str = 0;
      baddr = argv[0];
      bport = str + 1;
    }
  }
  for(str = argv[1]; *str; ++str) {
    if(*str == ':') {
      *str = 0;
      caddr = argv[1];
      cport = str + 1;
    }
  }

  struct addrinfo hints;
  struct addrinfo* baddrinfo;
  struct addrinfo* caddrinfo;

  /* Create the listening socket. */
  int sserv = socket(AF_INET, SOCK_STREAM, 0);
  if(sserv == -1) {
    perror("socket");
    return 1;
  }
  int one = 1;
  if(setsockopt(sserv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
    perror("setsockopt");
    return 1;
  }

  int wild = 0;
  if(baddr && !strcmp(baddr, "*")) {
    wild = 1;
    baddr = NULL;
  }

  /* Figure out the address to bind to and bind. */
  int res;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = demux || wild ? AI_PASSIVE : 0;
  if((res = getaddrinfo(baddr, bport, &hints, &baddrinfo))) {
    fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
    return 1;
  }
  if(baddrinfo == NULL) {
    fprintf(stderr, "Could not get bind address\n");
    return 1;
  }
  if(bind(sserv, baddrinfo->ai_addr, baddrinfo->ai_addrlen)) {
    perror("bind");
    return 1;
  }
  freeaddrinfo(baddrinfo);

  if(daemon) {
    pid_t pid = fork();
    if(pid == -1) {
      perror("fork (failed to daemonize");
      return 1;
    }
    if(pid != 0) {
      return 0;
    }
    if(-1 == setsid()) {
      return 1;
    }
    freopen("/dev/null", "r", stdin);
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
  }

  int iter;
  for(iter = 0; ; iter += iter < 7) {
    if(iter) {
      struct timespec ts;
      ts.tv_sec = 1 << iter;
      ts.tv_nsec = 0;
      nanosleep(&ts, NULL);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if((res = getaddrinfo(caddr, cport, &hints, &caddrinfo))) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
      if(retry_dns) {
        continue;
      } else {
        return 1;
      }
    }
    if(caddrinfo == NULL) {
      fprintf(stderr, "Could not get connect address\n");
      if(retry_dns) {
        continue;
      } else {
        return 1;
      }
    }
    break;
  }

  /* Set the server socket listening (and control socket if needed). */
  if(listen(sserv, 16)) {
    perror("listen");
    return 1;
  }

  res = muxloop(sserv, demux, caddrinfo);
  fprintf(stderr, "mux loop unexpectedly exited\n");
  return res;
}

static int muxloop(int sserv, int demux, struct addrinfo* caddrinfo) {
  struct epoll_event events[MAX_EVENTS];

  int epollfd = epoll_create(10);
  if(epollfd == -1) {
    perror("epoll_create");
    return 1;
  }

  MuxContext* ctx = new MuxContext(epollfd, demux);
  ctx->connector = new SocketConnector(ctx, caddrinfo);
  if(demux) {
    ctx->factory = new MuxStreamFactory(ctx, ctx->connector);
    ctx->linkserv = new LinkServer(ctx, sserv);
  } else {
    ctx->netlink = new NetlinkMonitor(ctx);
    ctx->muxserv = new MuxServer(ctx, sserv);

    Stream* sstream = ctx->connector->connect();
    ctx->lstream = new LinkStream(ctx, sstream);
    MuxStream* mstream = new MuxStream(ctx, ctx->lstream, NULL);
    ctx->lstream->attach_stream(mstream);
    sstream->attach_stream(ctx->lstream);
  }

  for(;;) {
    int nfds = TEMP_FAILURE_RETRY(
                epoll_wait(epollfd, events, MAX_EVENTS, MUX_TIMEOUT));

    struct epoll_event* ei,* ee;
    for(ei = events, ee = events + nfds; ei != ee; ++ei) {
      EventObject* eo = reinterpret_cast<EventObject*>(ei->data.ptr);
      if(ei->events & EPOLLIN) {
        eo->read();
      }
      if(ei->events & EPOLLOUT) {
        eo->write();
      }
    }
    for(typeof(ctx->delete_list.begin()) it = ctx->delete_list.begin();
        it != ctx->delete_list.end(); ++it) {
      delete *it;
    }
    ctx->delete_list.clear();
  }
}
