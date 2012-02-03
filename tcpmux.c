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

static int muxloop(int sserv, int demux, struct addrinfo* caddrinfo, int sctl);

static int setnonblocking(int s) {
  assert(s != -1);

  int flags;
  flags = fcntl(s, F_GETFL, 0);
  if(flags == -1 ||
     fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1) {
    perror("fcntl");
    return 1;
  }
  return 0;
}

static int socket_connected(int s) {
  assert(s != -1);

  int err;
  socklen_t errlen = sizeof(err);
  if(getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &errlen)) {
    perror("getsockopt");
    return 0;
  }
  return err == 0;
}

int main(int argc, char** argv) {
  srand(time(NULL));
  signal(SIGPIPE, SIG_IGN);

  char* str;
  int demux = 0;
  int retry_dns = 0;
  char* ctladdr = NULL;
  char* ctlport = NULL;
  if(*argv) for(++argv, --argc; *argv && (*argv)[0] == '-'; ++argv, --argc) {
    if(!strcmp("--demux", *argv)) {
      demux = 1;
    } else if(!strcmp("--retry", *argv)) {
      retry_dns = 1;
    } else if(!strcmp("--control", *argv) && argc > 1) {
      ctlport = *++argv;
      argc--;
      for(str = argv[0]; *str; ++str) {
        if(*str == ':') {
          *str = 0;
          ctladdr = argv[0];
          ctlport = str + 1;
        }
      }
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
    printf("  --control [control_bind_addr:]control_bind_port : "
           "Start control server\n");
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
  struct addrinfo* ctladdrinfo;

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

  /* Set the socket listening. */
  if(listen(sserv, 16)) {
    perror("listen");
    return 1;
  }

  int sctl = -1;
  if(ctlport) {
    sctl = socket(AF_INET, SOCK_STREAM, 0);
    if(sctl == -1) {
      perror("socket");
      return 1;
    }
    int one = 1;
    if(setsockopt(sctl, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one))) {
      perror("setsockopt");
      return 1;
    }

    int wild = 0;
    if(ctladdr && !strcmp(ctladdr, "*")) {
      wild = 1;
      ctladdr = NULL;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = wild ? AI_PASSIVE : 0;
    if((res = getaddrinfo(ctladdr, ctlport, &hints, &ctladdrinfo))) {
      fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
      return 1;
    }
    if(ctladdrinfo == NULL) {
      fprintf(stderr, "Could not get bind address\n");
      return 1;
    }
    if(bind(sctl, ctladdrinfo->ai_addr, ctladdrinfo->ai_addrlen)) {
      perror("bind");
      return 1;
    }
    freeaddrinfo(ctladdrinfo);

    /* Set the socket listening. */
    if(listen(sctl, 16)) {
      perror("listen");
      return 1;
    }
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

  res = muxloop(sserv, demux, caddrinfo, sctl);
  fprintf(stderr, "mux loop unexpectedly exited\n");
  return res;
}

#define KEYSZ 32
#define MAX_EVENTS 128
#define GLOBAL_KARMA (1 << 20)
#define CLIENT_KARMA (1 << 16)
#define VPACKETSIZE (1 << 10)
#define MAX_CLIENTS (1 << 16)
#define ACK_THRESHOLD (1 << 8)

struct mux_context;

typedef struct client_data {
  int is_client; /* Must be at the beginning of the struct.  Must be 1. */
  int is_connecting;
  int is_burned;
  int is_rdead;
  struct mux_context* context;

  int s;
  int id; /* Stored in network order. */
  int wkarma; /* Tracks how many more un-ack'ed bytes this client can send. */
  int rkarma; /* Tracks how many more bytes to ack. */
  struct client_data* next; /* Next client to write data. */
  struct client_data* karma_next; /* Next client to ack packets. */
  struct client_data* burn_next; /* Next client to free */

  int out_pos;
  int out_sz;
  char out_buf[CLIENT_KARMA];
} client_data;

typedef struct message {
  int id;
  int sz;
  char buf[VPACKETSIZE];
} message;

typedef struct mux_context {
  int is_client; /* Must be at the beginning of the struct.  Must be 0. */
  int is_connecting;
  int is_burned;
  int handshake_st;

  int demux;
  int epollfd;
  struct addrinfo* caddrinfo;

  int mainfd;
  client_data* write_head;
  client_data* write_tail;
  client_data* karma_head;
  client_data* karma_tail;
  client_data* burn_list;

  int client_table_size;
  client_data** client_table;

  char key[KEYSZ];

  /* Stream information. */
  int stream_karma; /* Amount of un-ack'ed bytes left to send. */
  int stream_pos; /* Position in circular buffer of last written byte. */
  int stream_debt; /* Amount of stream that needs to be resent. */
  char stream[GLOBAL_KARMA];

  /* Reverse stream information. */
  int rstream_pos; /* Position in logical circular buffer of last read byte. */
  int rstream_karma; /* Amount of bytes to ack. */

  /* Write state information. */
  int wpos;
  int wsz;
  message wout;

  /* Read state information. */
  int read_bytes;
  int rpos;
  message rin;

#ifndef NO_ZIP
  z_stream wstrm;
  z_stream rstrm;
#endif

  struct mux_context* next_context;
  struct mux_context* prev_context;
} mux_context;

static mux_context* context_list;

static int mainw(mux_context* mc);
static int mainr(mux_context* mc);
static int clientw(client_data* cd);
static int clientr(client_data* cd);

static int initiate_client_connect(client_data* cd) {
  VLOG("Starting client connection");

  mux_context* mc = cd->context;
  int epollfd = mc->epollfd;

  if(cd->s != -1) close(cd->s);
  cd->is_connecting = 1;
  cd->s = socket(AF_INET, SOCK_STREAM, 0);
  setnonblocking(cd->s);
  if(connect(cd->s, mc->caddrinfo->ai_addr, mc->caddrinfo->ai_addrlen)) {
    if(errno != EINPROGRESS) {
      perror("connect");
      return 1;
    }
  } else {
    /* Uncommon case (does it ever happen).  Connection finished immediately. */
    VLOG("Client quick connect");
    cd->is_connecting = 0;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.ptr = cd;
  if(epoll_ctl(epollfd, EPOLL_CTL_ADD, cd->s, &ev)) {
    perror("epoll_ctl");
    return 1;
  }
  return 0;
}

static int push_writer(mux_context* mc, client_data* cd, int from_mainw) {
  if(cd->next) return 0; /* This shouldn't happen often. */
  if(mc->write_head) {
    mc->write_tail->next = cd;
    mc->write_tail = cd;
  } else {
    mc->write_head = mc->write_tail = cd;

    /* Try pushing some data out if possible. */
    return from_mainw ? 0 : mainw(mc);
  }
  return 0;
}

static void set_rkarma(client_data* cd, int nkarma, int from_mainw) {
  mux_context* mc = cd->context;
  int noqueue = 0 <= cd->rkarma && cd->rkarma < ACK_THRESHOLD;
  cd->rkarma = nkarma;
  if(noqueue && (nkarma == -1 || ACK_THRESHOLD <= nkarma)) {
    assert(cd->karma_next == NULL);
    if(mc->karma_head) {
      mc->karma_tail->karma_next = cd;
      mc->karma_tail = cd;
    } else {
      mc->karma_head = mc->karma_tail = cd;
      if(mc->rstream_karma < ACK_THRESHOLD) {
        push_writer(mc, *mc->client_table, from_mainw);
      }
    }
  }
}

static int disconnect_client(client_data* cd, int from_mainw) {
  VLOG("Disconnecting client");
  mux_context* mc = cd->context;
  if(epoll_ctl(mc->epollfd, EPOLL_CTL_DEL, cd->s, NULL)) {
    perror("epoll_ctl");
    return 1;
  }
  close(cd->s);
  cd->s = -1;
  set_rkarma(cd, -1, from_mainw);
  return 0;
}

static client_data* allocate_client(mux_context* mc, int nid) {
  /* This naieve implementation should be ok for less than 1,000 connections. */
  if(nid == -1) {
    for(nid = 0; nid < mc->client_table_size && mc->client_table[nid]; nid++);
  }
  while(nid >= mc->client_table_size) {
    int nsz = mc->client_table_size * 3 / 2 + 4;
    mc->client_table = (client_data**)
        realloc(mc->client_table, sizeof(client_data*) * nsz);
    if(!mc->client_table) {
      fprintf(stderr, "Failed to allocate client table %d\n", nsz);
      return NULL;
    }
    memset(mc->client_table + mc->client_table_size, 0,
           sizeof(client_data*) * (nsz - mc->client_table_size));
    mc->client_table_size = nsz;
  }
  if(mc->client_table[nid]) {
    fprintf(stderr, "Assigned id already in use\n");
    return NULL;
  }
  client_data* cd = calloc(1, sizeof(client_data));
  if(!cd) {
    fprintf(stderr, "Could not allocate client\n");
    return NULL;
  }

  cd->is_client = 1;
  cd->context = mc;
  cd->id = nid;
  cd->s = -1;
  cd->wkarma = CLIENT_KARMA;
  return mc->client_table[nid] = cd;
}

client_data* get_client(mux_context* mc, int id, int from_mainw) {
  if(id < 0 || id >= MAX_CLIENTS) return NULL;
  if(id < mc->client_table_size && mc->client_table[id]) {
    return mc->client_table[id];
  }
  if(!mc->demux) {
    fprintf(stderr, "Got unexpected client id %d/%d\n",
            id, mc->client_table_size);
    return NULL;
  }

  client_data* cd = allocate_client(mc, id);
  if(initiate_client_connect(cd)) {
    disconnect_client(cd, from_mainw);
  }
  cd->wkarma = 0;
  return cd;
}

mux_context* make_context(int demux, int epollfd, struct addrinfo* caddrinfo) {
  mux_context* mc = calloc(1, sizeof(mux_context));
  if(!mc) {
    fprintf(stderr, "Could not allocate context\n");
    return NULL;
  }
  if(context_list == NULL) {
    context_list = mc->next_context = mc->prev_context = mc;
  } else {
    mc->next_context = context_list;
    mc->prev_context = context_list->prev_context;
    mc->next_context->prev_context = mc;
    mc->prev_context->next_context = mc;
  }
  mc->demux = demux;
  mc->epollfd = epollfd;
  mc->caddrinfo = caddrinfo;
  mc->mainfd = -1;
  mc->stream_karma = GLOBAL_KARMA - 1;
  allocate_client(mc, 0); /* Create the dummy control client. */
  return mc;
}

static void free_context(mux_context* mc) {
  // TODO: Deep free
  free(mc);
}

static int generate_key(mux_context* mc) {
  int i = 0;
  for(i = 0; i < KEYSZ; i++) {
    mc->key[i] = rand() & 0xFF;
  }
  return 0;
}

static void write_cbuf(char* cdata, int opos, int* csz, int cmxsz,
                       const char* wdata, int wsz, int rotsz) {
  while(wsz > 0) {
    int cpos = opos + *csz;
    cpos -= cpos >= cmxsz ? cmxsz : 0;

    int amt = cmxsz - cpos;
    amt = wsz < amt ? wsz : amt;
    memcpy(cdata + cpos, wdata, amt);
    
    wdata += amt;
    wsz -= amt;
    *csz += amt;
    if(rotsz && *csz >= cmxsz) *csz -= cmxsz;
  }
}

static int write_mux_header(mux_context* mc) {
  /* Write out the initial connecting header.  Because this operation
   * corresponds to a new connection we expect that we can write it out fully
   * without blocking. */
  int bpos = htonl(mc->rstream_pos);
  if((!mc->demux &&
       write(mc->mainfd, mc->key, sizeof(mc->key)) != sizeof(mc->key)) ||
      write(mc->mainfd, &bpos, sizeof(int)) != sizeof(int)) {
    fprintf(stderr, "Writting initial header unexpectedly failed\n");
    return 1;
  }
  return 0;
}

static int initiate_main_connect(mux_context* mc) {
  int epollfd = mc->epollfd;

  if(mc->mainfd != -1) close(mc->mainfd);
  mc->mainfd = socket(AF_INET, SOCK_STREAM, 0);
  mc->is_connecting = 1;
  mc->handshake_st = KEYSZ;

  if(setnonblocking(mc->mainfd)) return 1;

  int res;
  while((res = connect(mc->mainfd, mc->caddrinfo->ai_addr,
                       mc->caddrinfo->ai_addrlen))) {
    if(errno == EINPROGRESS) {
      break;
    } else if(errno == ENETUNREACH) {
      struct timespec ts;
      ts.tv_sec = 8;
      ts.tv_nsec = 0;
      nanosleep(&ts, NULL);
    } else {
      perror("connect");
      return 1;
    }
  }
  if(res == 0) {
    /* Uncommon case (does it ever happen?).  Connection finished
     * immediately. */
    mc->is_connecting = 0;
    write_mux_header(mc);
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
  ev.data.ptr = mc;
  if(epoll_ctl(epollfd, EPOLL_CTL_ADD, mc->mainfd, &ev)) {
    perror("epoll_ctl");
    return 1;
  }
  return 0;
}

static ssize_t write_mainfd(mux_context* mc, const void* buf, size_t count) {
  VVLOG("Writing data to mainfd");
  ssize_t amt = write(mc->mainfd, buf, count);
  if(amt < 0) {
    return amt;
  }
  mc->stream_karma -= amt;
  write_cbuf(mc->stream, 0, &mc->stream_pos, GLOBAL_KARMA, buf, amt, 1);
  return amt;
}

static void read_ack(mux_context* mc) {
  VVLOG("Reading ACK");

  int* data = (int*)mc->rin.buf;
  int* edata = (int*)(mc->rin.buf + mc->rin.sz);
  mc->stream_karma += ntohl(*data++);
  // TODO: Shouldn't be an assert.
  assert(0 <= mc->stream_karma && mc->stream_karma <= GLOBAL_KARMA);
  for(; data + 2 <= edata; ) {
    int id = ntohl(*data++);
    int karma = ntohl(*data++);
    client_data* cd = get_client(mc, id, 0);
    if(cd == NULL) {
      VLOG("Missing client?\n");
      continue;
    }

    if(karma >= 0) {
      cd->wkarma += karma;
      if(!cd->is_connecting && cd->wkarma == karma) {
        /* We just made room for this client to send more data.  Give it a
         * chance to write mroe data. */
        push_writer(mc, cd, 0);
      }
    } else {
      /* This is actually a disconnect message. */
      if(cd->s != -1) {
        /* We still thought the client was up.  Disconnect it and mark it as
         * disconnected. */
        cd->is_rdead = 1;
        disconnect_client(cd, 0);
      } else if(cd->rkarma == 0) {
        /* We knew the client was down and have already sent notice of this. */
        mc->client_table[id] = NULL;
        cd->is_burned = 1;
        cd->burn_next = mc->burn_list;
        mc->burn_list = cd;
      } else {
        /* We knew the client was down but haven't sent notice yet.  Mark it so
         * that when notice is sent it will be removed. */
        cd->is_rdead = 1;
      }
    }
  }
}

static void write_ack(mux_context* mc) {
  VVLOG("Writing ACK");
  assert(0 <= mc->rstream_karma && mc->rstream_karma <= GLOBAL_KARMA);
  assert(ACK_THRESHOLD <= mc->rstream_karma || mc->karma_head);

  int* data = (int*)mc->wout.buf;
  int* edata = (int*)(mc->wout.buf + sizeof(mc->wout.buf));
  *(data++) = htonl(mc->rstream_karma);
  mc->rstream_karma = 0;
  for(; mc->karma_head && data + 2 <= edata; ) {
    client_data* cd = mc->karma_head;
    assert(cd->rkarma == -1 ||
           (ACK_THRESHOLD <= cd->rkarma && cd->rkarma <= CLIENT_KARMA));
    assert(!cd->is_rdead || cd->rkarma == -1);
    *data++ = htonl(cd->id);
    *data++ = htonl(cd->rkarma);
    cd->rkarma = 0;
    mc->karma_head = cd->karma_next;
    cd->karma_next = NULL;
    
    if(cd->is_rdead) {
      /* If the client was marked as disconnected burn the client. */
      mc->client_table[cd->id] = NULL;
      cd->is_burned = 1;
      cd->burn_next = mc->burn_list;
      mc->burn_list = cd;
    }
  }
  mc->wout.id = -1;
  mc->wout.sz = (char*)data - mc->wout.buf;
}

static int disconnect_main(mux_context* mc) {
  VLOG("Disconnecting main");
  if(epoll_ctl(mc->epollfd, EPOLL_CTL_DEL, mc->mainfd, NULL)) {
    perror("epoll_ctl");
    return 1;
  }
  close(mc->mainfd);
  mc->mainfd = -1;
  return mc->demux ? 0 : initiate_main_connect(mc);
}

static int mainr(mux_context* mc) {
  if(mc->is_burned || mc->mainfd == -1 || mc->is_connecting) return 0;

  int hdr = sizeof(message) - VPACKETSIZE;
  while(1) {
    void* buf;
    size_t count;
    int rsz = -1;

    if(mc->handshake_st < KEYSZ) {
      buf = mc->key + mc->handshake_st;
      count = KEYSZ - mc->handshake_st;
    } else if(mc->handshake_st < KEYSZ + (int)sizeof(int)) {
      buf = ((char*)&mc->stream_debt) + mc->handshake_st - KEYSZ;
      count = sizeof(int) + KEYSZ - mc->handshake_st;
    } else {
      rsz = hdr;
      if(rsz <= mc->rpos) {
        rsz += ntohl(mc->rin.sz);
        if(ntohl(mc->rin.sz) == 0 || ntohl(mc->rin.sz) > VPACKETSIZE) {
          fprintf(stderr, "Unexpected virtual packet size\n");
          return 1;
        }
      }
      buf = ((char*)&mc->rin) + mc->rpos;
      count = rsz - mc->rpos;
    }

    ssize_t amt = read(mc->mainfd, buf, count);
    if(amt == 0 || (amt == -1 && errno != EAGAIN)) {
      return disconnect_main(mc);
    } else if(amt == -1) {
      return 0;
    }

    if(rsz == -1) {
      mc->handshake_st += amt;
      if(mc->handshake_st == KEYSZ) {
        /* Key transfer has finished.  Switch into existing context if it
         * exists. */
        mux_context* ctx = context_list;
        do {
          if(ctx->handshake_st == KEYSZ + sizeof(int) &&
             !memcmp(mc->key, ctx->key, KEYSZ)) {
            VLOG("Key matchup");
            if(ctx->mainfd != -1) {
              int res = disconnect_main(ctx);
              if(res) return 1;
            }

            ctx->handshake_st = mc->handshake_st;
            ctx->mainfd = mc->mainfd;
            mc->is_burned = 1;
            mc = ctx;

            struct epoll_event ev;
            memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
            ev.data.ptr = mc;
            if(epoll_ctl(mc->epollfd, EPOLL_CTL_MOD, mc->mainfd, &ev)) {
              perror("epoll_ctl");
              return 1;
            }
            break;
          }
          ctx = ctx->next_context;
        } while(ctx != context_list);
        write_mux_header(mc);
      } else if(mc->handshake_st == KEYSZ + sizeof(int)) {
        mc->stream_debt = mc->stream_pos - ntohl(mc->stream_debt);
        mc->stream_debt += mc->stream_debt < 0 ? GLOBAL_KARMA : 0;
        mainw(mc); /* Give us a chance to write our debts now. */
      }
      continue;
    }

    mc->rstream_pos += amt;
    if(mc->rstream_pos >= GLOBAL_KARMA) {
      mc->rstream_pos -= GLOBAL_KARMA;
    }

    /* Check if we have new data to acknowledge. */
    mc->rstream_karma += amt;
    mc->rstream_pos -= mc->rstream_pos >= GLOBAL_KARMA ? GLOBAL_KARMA : 0;
    if(!mc->karma_head &&
       mc->rstream_karma - amt < ACK_THRESHOLD &&
       mc->rstream_karma >= ACK_THRESHOLD) {
      push_writer(mc, mc->client_table[0], 0);
    }

    mc->rpos += amt;
    if(rsz > hdr && mc->rpos == rsz) {
      mc->rpos = 0;
      mc->rin.id = ntohl(mc->rin.id);
      mc->rin.sz = ntohl(mc->rin.sz);

      /* Got a packet!  Time to process it... */
      if(mc->rin.id == -1) {
        /* It's a control packet. */
        read_ack(mc);
      } else {
        client_data* cd = get_client(mc, mc->rin.id, 0);
        if(!cd) return 1;

        if((int)sizeof(cd->out_buf) - cd->out_sz < mc->rin.sz) {
          fprintf(stderr, "No room for incoming packet (karma error)\n");
          return 1;
        }
        write_cbuf(cd->out_buf, cd->out_pos, &cd->out_sz, sizeof(cd->out_buf),
                   mc->rin.buf, mc->rin.sz, 0);

        /* Let the client have a chance to write out data. */
        int res = clientw(cd);
        if(res) return res;
      }
    }
  }
}

static int mainw(mux_context* mc) {
  if(mc->is_burned || mc->mainfd == -1 || mc->is_connecting) return 0;

  /* Make sure we're not still doing a handshake. */
  if(mc->handshake_st < KEYSZ + (int)sizeof(int)) return 0;

  while(mc->write_head || mc->stream_debt > 0) {
    while(mc->stream_debt > 0) {
      VLOG("Absolving stream debt");
      int pos = mc->stream_pos - mc->stream_debt;
      int sz = mc->stream_debt;
      if(pos < 0) {
        pos += GLOBAL_KARMA;
        sz -= mc->stream_pos;
      }
      ssize_t amt = write(mc->mainfd, mc->stream + pos, sz);
      if(amt == 0 || (amt == -1 && errno != EAGAIN)) {
        return disconnect_main(mc);
      } else if(amt == -1) {
        return 0;
      }
      mc->stream_debt -= amt;
    }

    while(mc->wout.id == 0) {
      VLOG("Grabbing data to write");

      client_data* cd = mc->write_head;
      if(cd == NULL) {
        /* Nobody has anything to write. */
        return 0;
      }
      mc->write_head = mc->write_head->next;
      cd->next = NULL;

      int buffer_empty = 0;
      mc->wout.sz = 0;

      /* Grab as much data as we can up to the capacity of our vpacket. */
      if(cd->id != 0 && (cd->is_burned || cd->s == -1)) {
        /* This client is dead... just move past. */
        buffer_empty = 1;
      } else if(cd->id == 0) {
        write_ack(mc);
      } else {
        mc->wout.sz = 0;
        mc->wout.id = cd->id;
        int mxsz = VPACKETSIZE < cd->wkarma ? VPACKETSIZE : cd->wkarma;
        while(mc->wout.sz < mxsz) {
          ssize_t amt = read(cd->s, mc->wout.buf + mc->wout.sz,
                             mxsz - mc->wout.sz);
          if(amt <= 0) {
            buffer_empty = 1;
            if(mc->wout.sz == 0 &&
               (amt == 0 || (amt == -1 && errno != EAGAIN))) {
              int res = disconnect_client(cd, 1);
              if(res) return res;
            }
            break;
          }
          mc->wout.sz += amt;
        }
        cd->wkarma -= mc->wout.sz;
      }

      /* If there is more data to send and we have karma left put back on the
       * queue.  Never requeue the control 'client'. */
      if((!buffer_empty && cd->wkarma > 0 && cd->id > 0) ||
          (cd->id == 0 && mc->karma_head)) {
        assert(mc->wout.sz != 0);
        if(mc->write_head) {
          mc->write_tail->next = cd;
          mc->write_tail = cd;
        } else {
          mc->write_head = mc->write_tail = cd;
        }
      }

      if(mc->wout.sz == 0) {
        mc->wout.id = 0;
        continue;
      }
      mc->wpos = 0;
      mc->wsz = sizeof(message) - VPACKETSIZE + mc->wout.sz;
      mc->wout.id = htonl(mc->wout.id);
      mc->wout.sz = htonl(mc->wout.sz);
    }

    /* Dump out as much data as we can.  If the write buffer fills up just back
     * out. */
    while(mc->wpos < mc->wsz) {
      int numb = mc->stream_karma;
      if(numb == 0) {
        /* No global karma left. */
        return 0;
      }
      numb = mc->wsz - mc->wpos < numb ? mc->wsz - mc->wpos : numb;
      ssize_t amt = write_mainfd(mc, ((char*)&mc->wout) + mc->wpos, numb);
      if(amt == 0 || (amt == -1 && errno != EAGAIN)) {
        return disconnect_main(mc);
      } else if(amt == -1) {
        return 0;
      }
      mc->wpos += amt;
    }
    mc->wout.id = 0;
  }
  return 0;
}

static int clientr(client_data* cd) {
  if(cd->is_burned || cd->s == -1 || cd->is_connecting) return 0;
  return push_writer(cd->context, cd, 0);
}

static int clientw(client_data* cd) {
  if(cd->is_burned || cd->s == -1 || cd->is_connecting) return 0;
  mux_context* mc = cd->context;
  while(cd->out_sz) {
    VVLOG("Writing client data");

    int sz = sizeof(cd->out_buf) - cd->out_pos;
    sz = cd->out_sz < sz ? cd->out_sz : sz;

    ssize_t amt = write(cd->s, cd->out_buf + cd->out_pos, sz);
    if(amt == 0 || (amt == -1 && errno != EAGAIN)) {
      return disconnect_client(cd, 0);
    } else if(amt == -1) {
      return 0;
    }

    int noqueue = 0 <= cd->rkarma && cd->rkarma < ACK_THRESHOLD;
    cd->rkarma += amt;
    cd->out_pos += amt;
    cd->out_pos -= cd->out_pos >= (int)sizeof(cd->out_buf) ?
                    sizeof(cd->out_buf) : 0;
    cd->out_sz -= amt;

    if(noqueue && ACK_THRESHOLD <= cd->rkarma) {
      assert(cd->karma_next == NULL);
      if(mc->karma_head) {
        mc->karma_tail->karma_next = cd;
        mc->karma_tail = cd;
      } else {
        mc->karma_head = mc->karma_tail = cd;
        if(mc->rstream_karma < ACK_THRESHOLD) {
          push_writer(mc, *mc->client_table, 0);
        }
      }
    }
  }
  return 0;
}

static int muxloop(int sserv, int demux, struct addrinfo* caddrinfo,
                   int sctl) {
  struct epoll_event ev, events[MAX_EVENTS];

  int epollfd = epoll_create(10);
  if(epollfd == -1) {
    perror("epoll_create");
    return 1;
  }

  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = NULL;
  if(epoll_ctl(epollfd, EPOLL_CTL_ADD, sserv, &ev)) {
    perror("epoll_ctl");
    return 1;
  }

  if(sctl != -1) {
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = &sctl;
    if(epoll_ctl(epollfd, EPOLL_CTL_ADD, sctl, &ev)) {
      perror("epoll_ctl");
      return 1;
    }
  }

  mux_context* mmc = NULL;
  if(!demux) {
    mux_context* mc = mmc = make_context(demux, epollfd, caddrinfo);
    if(!mc) return 1;
    if(generate_key(mc)) return 1;
    if(initiate_main_connect(mc)) return 1;
  }

  while(1) {
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if(nfds == -1) {
      if(errno == EINTR) {
        /* This is for testing purposes only. */
        printf("Dropping main connections\n");
        mux_context* mc = context_list;
        if(mc) do {
          disconnect_main(mc);
          mc = mc->next_context;
        } while(mc != context_list);
        continue;
      }
      perror("epoll_wait");
      return 1;
    }

    struct epoll_event* ei,* ee;
    for(ei = events, ee = events + nfds; ei != ee; ++ei) {
      if(ei->data.ptr == &sctl) {
        union {
          struct sockaddr_in addrin;
          struct sockaddr addr;
        } cli_addr;
        socklen_t cli_len = sizeof(cli_addr.addrin);
        int cs = accept(sctl, &cli_addr.addr, &cli_len);
        if(cs == -1) {
          perror("accept");
          return 1;
        }

        uint32_t rtt = 0xFFFFFFFFU;
        uint32_t rttvar = 0xFFFFFFFFU;
        struct tcp_info info;
        socklen_t tcp_info_length = sizeof(info);
        if(0 == getsockopt(context_list->mainfd ,SOL_TCP, TCP_INFO,
           &info, &tcp_info_length)) {
          rtt = info.tcpi_rtt;
          rttvar = info.tcpi_rttvar;
        }
        rtt = htonl(rtt);
        rttvar = htonl(rttvar);
        write(cs, &rtt, sizeof(rtt));
        write(cs, &rttvar, sizeof(rttvar));
        
        char buf[64];
        write(cs, buf, sprintf(buf, "\n%u %u\n", ntohl(rtt), ntohl(rttvar)));
        close(cs);
      } else if(!ei->data.ptr) {
        union {
          struct sockaddr_in addrin;
          struct sockaddr addr;
        } cli_addr;
        socklen_t cli_len = sizeof(cli_addr.addrin);
        int cs = accept(sserv, &cli_addr.addr, &cli_len);
        if(cs == -1) {
          perror("accept");
          return 1;
        }

        if(setnonblocking(cs)) return 1;
        if(demux) {
          /* Make a preliminary context.  We may match it up with an existing
           * context later. */
          VLOG("Got new connection.  Creating mux context...");
          mux_context* mc = make_context(demux, epollfd, caddrinfo);
          if(!mc) {
            return 1;
          }
          mc->mainfd = cs;
          ev.data.ptr = mc;
        } else {
          VLOG("Got new connection.  Creating client...");
          client_data* cd = allocate_client(mmc, -1);
          if(!cd) {
            return 1;
          }
          cd->s = cs;
          ev.data.ptr = cd;
          set_rkarma(cd, CLIENT_KARMA, 0);
        }
          
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        if(epoll_ctl(epollfd, EPOLL_CTL_ADD, cs, &ev)) {
          perror("epoll_ctl");
          return 1;
        }
      } else if(*(int*)ei->data.ptr) {
        /* It's a client. */
        client_data* cd = (client_data*)ei->data.ptr;
        if(cd->is_connecting) {
          if(socket_connected(cd->s)) {
            VLOG("Client connected");
            cd->is_connecting = 0;
          } else {
            int res = disconnect_client(cd, 0);
            //int res = initiate_client_connect(cd);
            if(res) return res;
            continue;
          }
        }
        int res;
        if(ei->events & EPOLLIN) {
          VVLOG("Client ready to read");
          if((res = clientr(cd))) return res;
        }
        if(ei->events & EPOLLOUT) {
          VVLOG("Client ready to write");
          if((res = clientw(cd))) return res;
        }
      } else {
        /* It's the main file descriptor. */
        mux_context* mc = (mux_context*)ei->data.ptr;
        if(mc->is_connecting) {
          if(socket_connected(mc->mainfd)) {
            mc->is_connecting = 0;
            write_mux_header(mc);
          } else {
            int res = initiate_main_connect(mc);
            if(res) return 1;
            continue;
          }
        }
        int res;
        if(ei->events & EPOLLIN) {
          VLOG("Main ready to read");
          if((res = mainr(mc))) return res;
        }
        if(ei->events & EPOLLOUT) {
          VLOG("Main ready to write");
          if((res = mainw(mc))) return res;
        }
      }
    }

    /* Free any clients or contexts on the burn list. */
    mux_context* mc = context_list;
    if(mc) do {
      client_data* cd = mc->burn_list;
      while(cd) {
        assert(cd->is_burned);
        client_data* ocd = cd->burn_next;
        free(cd);
        cd = ocd;
      }
      mc->burn_list = NULL;

      mux_context* nmc = mc->next_context;
      if(mc->is_burned) {
        if(context_list == mc) context_list = mc == nmc ? NULL : nmc;
        mc->next_context->prev_context = mc->prev_context;
        mc->prev_context->next_context = mc->next_context;
        free_context(mc);
      }
      mc = nmc;
    } while(context_list && mc != context_list);
  }
}
