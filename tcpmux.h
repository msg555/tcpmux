#ifndef TCPMUX_TCPMUX_H
#define TCPMUX_TCPMUX_H

#include "eventwatcher.h"

#include <list>
#include <vector>

#include <stddef.h>

#if DEBUG
#include <stdio.h>

#define DPRINTF(...) do {                                                     \
    fprintf(stderr, __VA_ARGS__);                                             \
    fflush(stderr);                                                           \
  } while(0)
#else
#define DPRINTF(...)
#endif

class LinkStream;
class NetlinkMonitor;
class LinkServer;
class MuxServer;
class StreamFactory;
class Connector;
class EventObject;
struct addrinfo;

struct MuxContext : public EventWatcher {
  MuxContext(int epollfd, bool demux)
      : lstream(NULL), netlink(NULL), linkserv(NULL), muxserv(NULL),
        factory(NULL), connector(NULL), epollfd(epollfd), demux(demux) {
  }

  virtual void add_descriptor(EventObject* obj, int fd, int opts);
  virtual void mod_descriptor(EventObject* obj, int fd, int opts);
  virtual void del_descriptor(EventObject* obj, int fd);

  LinkStream* lstream;
  NetlinkMonitor* netlink;
  LinkServer* linkserv;
  MuxServer* muxserv;
  StreamFactory* factory;
  Connector* connector;

  std::list<EventObject*> delete_list;

  int epollfd;
  bool demux;
};

bool socket_connected(int s);
bool setnonblocking(int s);
int initiate_connect(struct addrinfo* caddrinfo);
void reset_connecting_link(MuxContext* ctx);

#endif
