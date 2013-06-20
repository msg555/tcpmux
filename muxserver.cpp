#include "muxserver.h"
#include "muxstream.h"
#include "linkstream.h"
#include "socketobject.h"
#include "tcpmux.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

MuxServer::MuxServer(MuxContext* ctx, int s)
    : ctx(ctx), mstream(NULL), s(s) {
  setnonblocking(s);
  ctx->add_descriptor(this, s, 0);
}

MuxServer::~MuxServer() {
  close(s);
}

void MuxServer::associate_mux(MuxStream* mstream) {
  if(!mstream != !this->mstream) {
    DPRINTF("CHANGE DESC %d\n", mstream ? EventWatcher::READ : 0);
    ctx->mod_descriptor(this, s, mstream ? EventWatcher::READ : 0);
  }
  this->mstream = mstream;
}

void MuxServer::read() {
  if(mstream) for(;;) {
    int cs = TEMP_FAILURE_RETRY(accept(s, NULL, NULL));
    if(cs == -1) {
      break;
    }
    DPRINTF("Got new mux client on fd:%d\n", cs);

    mstream->attach_client(new SocketObject(ctx, cs));
  }
}
