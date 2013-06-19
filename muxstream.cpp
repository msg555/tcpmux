#include "muxserver.h"
#include "muxstream.h"
#include "tcpmux.h"

#include <algorithm>

#include <string.h>

#include <arpa/inet.h>

using std::min;

MuxStream::MuxStream(MuxContext* ctx, Stream* lower_link, Connector* connector)
    : ctx(ctx), lower_link(lower_link), connector(connector),
      ll_state(-4), ll_client(NULL), vpacket_pos(0), vpacket_size(0),
      write_head(NULL), write_tail(NULL) {
DPRINTF("MUXSTREAM CREATED %p\n", this);
  memset(clients, 0, sizeof(clients));
  if(ctx->muxserv) {
    assert(!connector);
    assert(ctx->muxserv);
    ctx->muxserv->associate_mux(this);
  }
}

MuxStream::~MuxStream() {
}

size_t MuxStream::push(Stream* source, const char* data, size_t count) {
for(size_t i = 0; i < count; i++) if(data[i] == 0) count=count;

  size_t initial_count = count;
  if(source == lower_link) {
    bool need_pop = false;
    for(; count > 0; ) {
      /* First read the virtual packet header. */
      if(ll_state < 0) {
        size_t amt = min(count, (size_t)-ll_state);
        memcpy(ll.all + 4 + ll_state, data, amt);
        ll_state += amt;
        data += amt; count -= amt;

        if(ll_state == 0) {
          ll.val.id = ntohs(ll.val.id);
          ll.val.len = ntohs(ll.val.len);
          ll_client = get_stream(ll.val.id);

          if(ll.val.len == 0) {
            /* 'Length 0' messages are actually acknowledgments of half the
             * maximum client buffer size of bytes. */
            assert(ll_client->unacked_bytes >= MUXSTREAM_CLIENT_BUFFER_SIZE/ 2);
            ll_client->unacked_bytes -= MUXSTREAM_CLIENT_BUFFER_SIZE / 2;
            ll_client->pop(this);
          }
        }
      }

      /* Next push the body of the data to the appropriate client stream. */
      if(0 <= ll_state) {
        size_t wamt = min(count, (size_t)ll.val.len - ll_state);
        size_t amt = ll_client->push(this, data, wamt);
        assert(amt == wamt && "muxed streams shouldn't outpace their buffers");

        ll_state += amt;
        data += amt; count -= amt;
        if(ll_state == ll.val.len) {
          ll_state = -4;
        }

        ll_client->received_bytes += amt;
        if(ll_client->received_bytes >= MUXSTREAM_CLIENT_BUFFER_SIZE / 2) {
          need_pop |= wants_write(ll_client);
        }
      }
    }
    if(need_pop) {
      pop(source);
    }
  } else {
    /* Prepare a virtual packet for transmission if nothing is already being
     * staged. */
    MuxedClient* client = static_cast<MuxedClient*>(source);
    for(; 0UL < count &&
          client->unacked_bytes < MUXSTREAM_CLIENT_BUFFER_SIZE; ) {
      if(vpacket_pos == vpacket_size) {
        size_t amt = min(MUXSTREAM_CLIENT_BUFFER_SIZE - client->unacked_bytes,
                         min(count, MUXSTREAM_MAX_VPACKET));

        vpacket_pos = 0;
        vpacket_size = amt + 4;
        reinterpret_cast<uint16_t*>(vpacket)[0] = htons(client->client_id);
        reinterpret_cast<uint16_t*>(vpacket)[1] = htons(amt);
        memcpy(vpacket + 4, data, amt);

        data += amt; count -= amt;
        client->unacked_bytes += amt;
        drain();
      } else {
        wants_write(client);
      }
    }
  }
  return initial_count - count;
}

bool MuxStream::wants_write(MuxedClient* client) {
  if(client->next) {
    return false;
  }
  if(!write_head) {
    write_head = write_tail = client;
  } else if(!client->next) {
    write_tail->next = client;
    write_tail = client;
  }
  client->next = client;
  return client == write_head;
}

bool MuxStream::pop(Stream* source) {
  if(source == lower_link) {
    for(drain(); write_head && vpacket_pos == vpacket_size; drain()) {
      MuxedClient* client = write_head;
      bool add = true;
      if(ll_client->received_bytes >= MUXSTREAM_CLIENT_BUFFER_SIZE / 2) {
        /* Send off an ACK for this client if we have enough data to ack. */
        ll_client->received_bytes -= MUXSTREAM_CLIENT_BUFFER_SIZE / 2;
      } else {
        /* Otherwise allow the client to send data. */
        add = client->pop(this);
      }
      write_head = client->next == client ? NULL : client->next;
      client->next = NULL;
      if(add) {
        wants_write(client);
      }
    }
    return vpacket_pos < vpacket_size || write_head != NULL;
  } else {
    assert(0 && "muxedclient's should not call to pop");
  }
}

void MuxStream::drain() {
  if(vpacket_pos < vpacket_size) {
    vpacket_pos += lower_link->push(this, vpacket + vpacket_pos,
                                    vpacket_size - vpacket_pos);
  }
}

void MuxStream::attach_client(Stream* client_stream) {
  MuxedClient* client = new MuxedClient(this, client_stream);
  client_stream->set_forward(client);

  for(size_t i = 0; i < MAX_CLIENTS; i++) {
    if(!clients[i]) {
      clients[i] = client;
      client->client_id = i;
      return;
    }
  }
  assert(0 && "todo: too many clients");
}

MuxedClient* MuxStream::get_stream(uint16_t id) {
  assert(id < MAX_CLIENTS);
  if(connector && !clients[id]) {
    Stream* client_stream = connector->connect();
    MuxedClient* client = new MuxedClient(this, client_stream);
    client_stream->set_forward(client);
    client->client_id = id;
    clients[id] = client;
  }
  assert(clients[id] != NULL);
  return clients[id];
}

MuxedClient::MuxedClient(MuxStream* mux_stream, Stream* client_stream)
    : mux_stream(mux_stream), client_stream(client_stream), next(NULL),
      unacked_bytes(0), received_bytes(0), buf_pos(0), buf_size(0) {
}

MuxedClient::~MuxedClient() {
  delete client_stream;
}

size_t MuxedClient::push(Stream* source, const char* data, size_t count) {
  assert(source == client_stream || source == mux_stream);

  if(source == client_stream) {
    return mux_stream->push(this, data, count);
  }

  size_t result = 0;
  for(; 0 < count && buf_size < MUXSTREAM_CLIENT_BUFFER_SIZE; ) {
    size_t pos = (buf_pos + buf_size) & (MUXSTREAM_CLIENT_BUFFER_SIZE - 1);
    size_t amt = min(count, min(MUXSTREAM_CLIENT_BUFFER_SIZE - buf_size,
                                MUXSTREAM_CLIENT_BUFFER_SIZE - pos));
    memcpy(buf + pos, data, amt);
    data += amt; count -= amt;
    buf_size += amt;
    result += amt;
    drain();
  }

  return result;
}

bool MuxedClient::pop(Stream* source) {
  assert(source == client_stream || source == mux_stream);
  if(source == mux_stream) {
    return client_stream->pop(this);
  }

  /* There is no reason to up-call to mux_stream to pop data.  Any data
   * mux_stream might have availble will be pushed through immediately because
   * the client streams are required to have enough buffer space to absorb all
   * data thrown at them. */
  drain();

  return buf_size != 0;
}

void MuxedClient::drain() {
  for(; buf_size > 0; ) {
    size_t wamt = min(buf_size, MUXSTREAM_CLIENT_BUFFER_SIZE - buf_pos);
    size_t amt = client_stream->push(this, buf + buf_pos, wamt);
    buf_pos = (buf_pos + amt) & (MUXSTREAM_CLIENT_BUFFER_SIZE - 1);
    buf_size -= amt;
    if(amt != wamt) {
      break;
    }
  }
}
