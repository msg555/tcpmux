#ifndef TCPMUX_EVENTWATCHER_H
#define TCPMUX_EVENTWATCHER_H

class EventObject;

class EventWatcher {
 public:
  static const int READ = 1;
  static const int WRITE = 2;

  EventWatcher() {}
  virtual ~EventWatcher() {}

  virtual void add_descriptor(EventObject* obj, int fd, int opts) = 0;
  virtual void mod_descriptor(EventObject* obj, int fd, int opts) = 0;
  virtual void del_descriptor(EventObject* obj, int fd) = 0;
};

#endif
