#include "netlink.h"

#include "tcpmux.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

NetlinkMonitor::NetlinkMonitor(MuxContext* ctx) : ctx(ctx) {
  struct sockaddr_nl addr;
  s = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
  if(s == -1) {
    perror("socket NetlinkMonitor");
    return;
  }

  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_groups = RTMGRP_IPV4_IFADDR;
  if(bind(s, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
    perror("bind nl");
    return;
  }

  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = EPOLLIN;
  ev.data.ptr = this;
  if(epoll_ctl(ctx->epollfd, EPOLL_CTL_ADD, s, &ev)) {
    perror("epoll_ctl netlink");
    return;
  }
}

NetlinkMonitor::~NetlinkMonitor() {
  close(s);
}

void NetlinkMonitor::read() {
  int len;
  char buf[4096];
  bool new_addr = false;
  while((len = recv(s, &buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    for (; NLMSG_OK(nlh, (size_t)len) && nlh->nlmsg_type != NLMSG_DONE;
           nlh = NLMSG_NEXT(nlh, len)) {
      switch((int)nlh->nlmsg_type) {
        case RTM_NEWADDR:  case RTM_DELADDR:
        case RTM_NEWLINK:  case RTM_DELLINK:
        case RTM_NEWROUTE: case RTM_DELROUTE:
          new_addr = true;
          break;
      }
    }
  }
  if(new_addr) {
    /* Network interfaces have changed.  Let's try connecting again
     * to make sure we have the best interface. */
    reset_connecting_link(ctx);
  }
}

bool NetlinkMonitor::alive() {
  return s != -1;
}
