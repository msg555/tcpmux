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

  virtual size_t push(Stream* source, const char* buf, size_t count);
  virtual bool pop(Stream* source);

  virtual void attach_stream(Stream* stream);
  virtual void replace_stream(Stream* old_stream, Stream* new_stream);
  virtual void disconnect_stream(Stream* stream);

  virtual void read();
  virtual void write();

 private:
  void do_close();

  EventWatcher* watcher;
  int s;
  Stream* forward;
  int opts;

  size_t buf_pos;
  size_t buf_size;
  char buf[SOCKET_BUFFER_SIZE];
};

#endif
