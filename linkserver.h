#ifndef TCPMUX_LINKSERVER_H
#define TCPMUX_LINKSERVER_H

#include "eventobject.h"

class MuxContext;

class LinkServer : public EventObject {
 public:
  LinkServer(MuxContext* ctx, int s);
  virtual ~LinkServer();

  virtual void read();

 private:
  MuxContext* ctx;
  int s;
};

#endif
