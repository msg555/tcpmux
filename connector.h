#ifndef TCPMUX_CONNECTOR_H
#define TCPMUX_CONNECTOR_H

class EventWatcher;
class MuxContext;
class Stream;
struct addrinfo;

class Connector {
 public:
  Connector() {}
  virtual ~Connector() {}

  virtual Stream* connect() = 0;
};

class SocketConnector : public Connector {
 public:
  SocketConnector(EventWatcher* watcher, struct addrinfo* caddrinfo);
  virtual ~SocketConnector();

  virtual Stream* connect();

 private:
  EventWatcher* watcher;
  struct addrinfo* caddrinfo;
};

#endif
