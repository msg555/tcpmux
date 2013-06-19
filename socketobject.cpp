#include "eventwatcher.h"
#include "socketobject.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "tcpmux.h"

SocketObject::SocketObject(EventWatcher* watcher, int s)
    : watcher(watcher), s(s), forward(NULL), opts(0), buf_pos(0), buf_size(0) {
  if(s != -1) {
    opts = EventWatcher::READ | EventWatcher::WRITE;
    watcher->add_descriptor(this, s, opts);
  }
}

SocketObject::~SocketObject() {
  close_sock();
  delete forward;
}

void SocketObject::set_forward(Stream* forward) {
  this->forward = forward;
}

size_t SocketObject::push(Stream* source, const char* buf, size_t count) {
  assert(count != 0);

  size_t result = 0;
  for(; 0 < count; ) {
    ssize_t amt = TEMP_FAILURE_RETRY(send(s, buf, count, MSG_DONTWAIT));
DPRINTF("PUSHING SOCKET %p -> %zd/%zu\n", this, amt, count);
    if(amt <= 0) {
      if(!(amt == -1 && errno == EAGAIN)) {
        close_sock();
      }
      break;
    }
    buf += amt;
    count -= amt;
    result += amt;
  }
  return result;
}

bool SocketObject::pop(Stream* source) {
  for(;;) {
    if(buf_pos < buf_size) {
      buf_pos += forward->push(this, buf + buf_pos, buf_size - buf_pos);
DPRINTF("SOCKET PUSHED OUT %p -> %zu/%zu\n", this, buf_pos, buf_size);
      if(buf_pos != buf_size) {
        break;
      }
    } else {
      ssize_t amt = TEMP_FAILURE_RETRY(
                          recv(s, buf, SOCKET_BUFFER_SIZE, MSG_DONTWAIT));
      if(amt <= 0) {
        if(!(amt == -1 && errno == EAGAIN)) {
          close_sock();
        }
        break;
      }
      buf_pos = 0;
      buf_size = amt;
    }
  }
  return buf_size != 0;
}

void SocketObject::read() {
DPRINTF("GOT READ %p\n", this);
  pop(forward);

  int nopts = buf_pos == buf_size ? opts | EventWatcher::READ :
                                    opts & ~EventWatcher::READ;
  if(nopts != opts) {
    opts = nopts;
DPRINTF("CHANGING OPTS %d\n", opts);
    watcher->mod_descriptor(this, s, opts);
  }
}

void SocketObject::write() {
DPRINTF("GOT WRITE %p\n", this);
  bool more = forward->pop(this);

  int nopts = more ? opts | EventWatcher::WRITE : opts & ~EventWatcher::WRITE;
  if(nopts != opts) {
    opts = nopts;
    watcher->mod_descriptor(this, s, opts);
  }
}

bool SocketObject::alive() {
  return s != -1;
}

void SocketObject::close_sock() {
  if(s != -1) {
    close(s);
  }
  s = -1;
}
