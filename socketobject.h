#ifndef TCPMUX_SOCKETOBJECT_H
#define TCPMUX_SOCKETOBJECT_H

#include "stream.h"
#include "eventobject.h"

#include <stddef.h>

#define SOCKET_BUFFER_SIZE (1U << 10)

class EventWatcher;

class SocketObject : public Stream, EventObject {
 public:
  SocketObject(EventWatcher* watcher, int s);
  virtual ~SocketObject();

  void set_forward(Stream* forward);

  virtual size_t push(Stream* source, const char* buf, size_t count);
  virtual bool pop(Stream* source);

  virtual void read();
  virtual void write();

  virtual bool alive();

 private:
  void close_sock();

  EventWatcher* watcher;
  int s;
  Stream* forward;
  int opts;

  size_t buf_pos;
  size_t buf_size;
  char buf[SOCKET_BUFFER_SIZE];
};

#endif
