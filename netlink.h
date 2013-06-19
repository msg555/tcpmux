#ifndef TCPMUX_NETLINK_H
#define TCPMUX_NETLINK_H

#include "eventobject.h"

struct addrinfo;
class MuxContext;

class NetlinkMonitor : public EventObject {
 public:
  NetlinkMonitor(MuxContext* ctx);
  ~NetlinkMonitor();

  virtual void read();
  virtual bool alive();

 private:
  int s;
  MuxContext* ctx;
};

#endif
