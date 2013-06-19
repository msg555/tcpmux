#ifndef TCPMUX_MUXSTREAM_H
#define TCPMUX_MUXSTREAM_H

#include "connector.h"
#include "stream.h"
#include "streamfactory.h"

#include <stddef.h>
#include <stdint.h>

const size_t MUXSTREAM_MAX_VPACKET = 1UL << 12;
const size_t MUXSTREAM_CLIENT_BUFFER_SIZE = 1UL << 17;
const size_t MAX_CLIENTS = 1UL << 12;

class MuxContext;
class MuxedClient;

class MuxStream : public Stream {
 public:
  MuxStream(MuxContext* ctx, Stream* lower_link, Connector* connector);
  virtual ~MuxStream();

  virtual size_t push(Stream* source, const char* buf, size_t count);
  virtual bool pop(Stream* source);

  void attach_client(Stream* client_stream);

 private:
  void drain();
  MuxedClient* get_stream(uint16_t id);
  bool wants_write(MuxedClient* client);

  MuxContext* ctx;
  Stream* lower_link;
  Connector* connector;

  int ll_state;
  union {
    struct {
      uint16_t id;
      uint16_t len;
    } val;
    char all[4];
  } ll;
  MuxedClient* ll_client;

  size_t vpacket_pos;
  size_t vpacket_size;
  char vpacket[MUXSTREAM_MAX_VPACKET + 4];

  MuxedClient* write_head;
  MuxedClient* write_tail;
  MuxedClient* clients[MAX_CLIENTS];
};

class MuxedClient : public Stream {
 friend class MuxStream;

 public:
  MuxedClient(MuxStream* mux_stream, Stream* client_stream);
  virtual ~MuxedClient();

  virtual size_t push(Stream* source, const char* buf, size_t count);
  virtual bool pop(Stream* source);

 private:
  void drain();

  MuxStream* mux_stream;
  Stream* client_stream;
  MuxedClient* next;

  size_t client_id;
  size_t unacked_bytes;
  size_t received_bytes;

  size_t buf_pos;
  size_t buf_size;
  char buf[MUXSTREAM_CLIENT_BUFFER_SIZE];
};

class MuxStreamFactory : public StreamFactory {
 public:
  MuxStreamFactory(MuxContext* ctx, Connector* connector)
      : ctx(ctx), connector(connector) {
  }

  virtual ~MuxStreamFactory() {
    delete connector;
  }

  virtual Stream* create(Stream* lower_link) {
    return new MuxStream(ctx, lower_link, connector);
  }

 private:
  MuxContext* ctx;
  Connector* connector;
};

#endif
