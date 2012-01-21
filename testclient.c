#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

int transfer_loop(int s, int server) {
  int i, j;
  for(i = 1; i <= 1024; i++) {
    printf("Phase %d\n", i);

    int amt = 1024 * i;
    unsigned char* buf = malloc(amt);
    if(!buf) {
      fprintf(stderr, "Failed to allocate buffer\n");
      return 1;
    }
    for(j = 0; j < amt; j++) buf[j] = j & 0xFF;

    int pos = 0;
    while(pos < amt) {
      //ssize_t res = (!!server ^ i & 1) ? write(s, buf + pos, amt - pos) :
      //                                   read(s, buf + pos, amt - pos);
      ssize_t res = server ? write(s, buf + pos, amt - pos) :
                                         read(s, buf + pos, amt - pos);
      if(res == -1) {
        perror("write/read");
        return 1;
      } else if(res == 0) {
        fprintf(stderr, "Unexpected hang-up\n");
        return 1;
      }
      pos += res;
    }
    for(j = 0; j < amt; j++) {
      if(buf[j] != (j & 0xFF)) {
        fprintf(stderr, "Unexpected payload received %d %d\n", i, j);
        return 1;
      }
    }
    free(buf);
  }
  return 0;
}

int main(int argc, char** argv) {
  srand(time(NULL));

  int server = 0;
  int do_fork = 0;
  if(*argv) for(++argv, --argc; *argv && (*argv)[0] == '-'; ++argv, --argc) {
    if(!strcmp("--server", *argv)) {
      server = 1;
    } else if(!strcmp("--fork", *argv)) {
      do_fork = 1;
    }
  }

  if(argc != 1) {
    printf("testclient --server [bind_addr:]bind_port\n"
           "testclient connect_addr:connect_port\n\n");
    printf("  [optons]\n");
    printf("  --server  : Demultiplex mode (rather than multiplex)\n");
    return 0;
  }

  char* str;
  char* addr = NULL;
  char* port = argv[0];
  for(str = argv[0]; *str; ++str) {
    if(*str == ':') {
      *str = 0;
      addr = argv[0];
      port = str + 1;
    }
  }

  struct addrinfo hints;
  struct addrinfo* baddrinfo;

  /* Figure out the address to bind to and bind. */
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  if(getaddrinfo(addr, port, &hints, &baddrinfo)) {
    perror("getaddrinfo");
    return 1;
  }
  if(baddrinfo == NULL) {
    fprintf(stderr, "Could not get address\n");
    return 1;
  }

  if(server) {  
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

    while(1) {
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

      if(!do_fork || fork()) {
        close(sserv);
        return transfer_loop(cs, server);
      }
      close(cs);
    }
  } else {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if(s == -1) {
      perror("socket");
      return 1;
    }

    struct addrinfo* rp;
    for(rp = baddrinfo; rp; rp = rp->ai_next) {
      if(!connect(s, rp->ai_addr, rp->ai_addrlen)) {
        freeaddrinfo(baddrinfo);
        return transfer_loop(s, server);
      }
    }
    freeaddrinfo(baddrinfo);
    fprintf(stderr, "Failed to connect\n");
    return 1;
  }
}
