#ifndef TCPMUX_EVENTOBJECT_H
#define TCPMUX_EVENTOBJECT_H

class EventObject {
 public:
  EventObject() {}
  virtual ~EventObject() {}

  virtual void read() {}
  virtual void write() {}

  virtual bool alive() {
    return true;
  }
};

#endif
