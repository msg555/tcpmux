#include "eventwatcher.h"
#include "socketobject.h"
#include "tcpmux.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio.h>

SocketObject::SocketObject(EventWatcher* watcher, int s)
    : watcher(watcher), s(s), forward(NULL), opts(0), buf_pos(0), buf_size(0) {
  assert(watcher != NULL);
}

SocketObject::~SocketObject() {
  do_close();
}

void SocketObject::attach_stream(Stream* forward) {
  assert(this->forward == NULL && forward != NULL);
  this->forward = forward;
  if(s == -1) {
    do_close();
  } else {
    opts = EventWatcher::READ | EventWatcher::WRITE;
    watcher->add_descriptor(this, s, opts);
  }
}

size_t SocketObject::push(Stream* source, const char* buf, size_t count) {
  assert(count != 0);
  if(s == -1) {
    return 0;
  }

  size_t result = 0;
  for(; 0 < count; ) {
    ssize_t amt = TEMP_FAILURE_RETRY(send(s, buf, count, MSG_DONTWAIT));
    if(amt <= 0) {
      if(!(amt == -1 && errno == EAGAIN)) {
        if(amt != 0) {
          perror("send SocketObject::push");
        }
        do_close();
      }
      break;
    }
    buf += amt;
    count -= amt;
    result += amt;
  }
DPRINTF("SOCKET WRITE %zu/%zu\n", result, result + count);
  return result;
}

bool SocketObject::pop(Stream* source) {
  for(;;) {
    if(buf_pos < buf_size) {
      buf_pos += forward->push(this, buf + buf_pos, buf_size - buf_pos);
      if(buf_pos != buf_size) {
        break;
      }
    } else if(s != -1) {
      ssize_t amt = TEMP_FAILURE_RETRY(
                          recv(s, buf, SOCKET_BUFFER_SIZE, MSG_DONTWAIT));
      if(amt <= 0) {
        if(!(amt == -1 && errno == EAGAIN)) {
          if(amt != 0) {
            perror("recv SocketObject::push");
          }
          do_close();
        }
        break;
      }
      buf_pos = 0;
      buf_size = amt;
    } else {
      break;
    }
  }

  int nopts = buf_pos == buf_size ? opts | EventWatcher::READ :
                                    opts & ~EventWatcher::READ;
  if(nopts != opts) {
    opts = nopts;
    watcher->mod_descriptor(this, s, opts);
  }

  return buf_pos < buf_size;
}

void SocketObject::read() {
  if(!forward) return;

  pop(forward);
}

void SocketObject::write() {
  if(!forward) return;

  bool more = forward->pop(this);

  int nopts = more ? opts | EventWatcher::WRITE : opts & ~EventWatcher::WRITE;
  if(nopts != opts) {
    opts = nopts;
    watcher->mod_descriptor(this, s, opts);
  }
}

void SocketObject::replace_stream(Stream* old_stream, Stream* new_stream) {
  assert(forward == old_stream && new_stream != NULL);
  forward = new_stream;
}

void SocketObject::disconnect_stream(Stream* stream) {
  assert(forward == stream);
  forward = NULL;
  do_close();
}

void SocketObject::do_close() {
  if(s != -1) {
DPRINTF("SOCKET CLOSED %d\n", s);
    watcher->del_descriptor(this, s);
    close(s);
  }
  if(forward) {
    forward->disconnect_stream(this);
    forward = NULL;
  }
  s = -1;
}
