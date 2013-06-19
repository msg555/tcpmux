#ifndef TCPMUX_MUXSERVER_H
#define TCPMUX_MUXSERVER_H

#include "eventobject.h"

class MuxContext;
class MuxStream;

class MuxServer : public EventObject {
 public:
  MuxServer(MuxContext* ctx, int s);
  virtual ~MuxServer();

  void associate_mux(MuxStream* mstream);

  virtual void read();

 private:
  MuxContext* ctx;
  MuxStream* mstream;
  int s;
};

#endif
