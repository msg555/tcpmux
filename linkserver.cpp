#include "linkserver.h"
#include "linkstream.h"
#include "socketobject.h"
#include "tcpmux.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

LinkServer::LinkServer(MuxContext* ctx, int s) : ctx(ctx), s(s) {
  setnonblocking(s);
  ctx->add_descriptor(this, s, EventWatcher::READ);
}

LinkServer::~LinkServer() {
  close(s);
}

void LinkServer::read() {
  for(;;) {
    int cs = TEMP_FAILURE_RETRY(accept(s, NULL, NULL));
    if(cs == -1) {
      break;
    }
    DPRINTF("Got new link on fd:%d\n", cs);

    SocketObject* sstream = new SocketObject(ctx, cs);
    LinkStream* lstream = new LinkStream(ctx, sstream, ctx->factory);
    sstream->attach_stream(lstream);
  }
}
