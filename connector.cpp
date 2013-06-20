#include "connector.h"
#include "socketobject.h"
#include "tcpmux.h"

#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <stdio.h>
#include <time.h>

SocketConnector::SocketConnector(EventWatcher* watcher,
                                 struct addrinfo* caddrinfo)
    : watcher(watcher), caddrinfo(caddrinfo) {
}

SocketConnector::~SocketConnector() {
}

Stream* SocketConnector::connect() {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  setnonblocking(s);

  int res;
  while((res = ::connect(s, caddrinfo->ai_addr, caddrinfo->ai_addrlen))) {
    if(errno == EINPROGRESS) {
      break;
    } else if(errno == ENETUNREACH) {
      // TODO: Queue netlink to trigger a retry
      struct timespec ts;
      ts.tv_sec = 8;
      ts.tv_nsec = 0;
      nanosleep(&ts, NULL);
    } else {
      perror("connect");
    }
  }

  return new SocketObject(watcher, s);
}
