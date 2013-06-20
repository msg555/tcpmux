#include "muxserver.h"
#include "muxstream.h"
#include "tcpmux.h"

#include <algorithm>

#include <string.h>

#include <arpa/inet.h>

using std::min;

static const uint16_t DELETE_ID_BIT = 1U << 15;

MuxStream::MuxStream(MuxContext* ctx, Stream* lower_link, Connector* connector)
    : ctx(ctx), lower_link(lower_link), connector(connector),
      ll_state(-4), ll_client(NULL), vpacket_pos(0), vpacket_size(0),
      write_head(NULL), write_tail(NULL) {
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
          ll_client = get_stream(ll.val.id & ~DELETE_ID_BIT);

          if(ll.val.id & DELETE_ID_BIT) {
            /* The delete bit indicates the stream has been disconnected. */
            if(ll_client->client_id & DELETE_ID_BIT) {
              /* If we already sent a disconnect message delete now. */
              assert(ll_client->client_stream == NULL);
              clients[ll.val.id & ~DELETE_ID_BIT] = NULL;
              delete ll_client;
            } else if(ll_client->client_stream) {
              /* Otherwise disconnect and trigger a message. */
              assert(ll_client->client_stream != NULL);
              ll_client->client_id |= DELETE_ID_BIT;
              ll_client->client_stream->disconnect_stream(ll_client);
              ll_client->client_stream = NULL;
              wants_disconnect(ll_client);
            }
            ll_state = -4;
          } else if(ll.val.len == 0) {
            /* 'Length 0' messages are actually acknowledgments of half the
             * maximum client buffer size of bytes. */
            assert(ll_client->unacked_bytes >= MUXSTREAM_CLIENT_BUFFER_SIZE/ 2);
            ll_client->unacked_bytes -= MUXSTREAM_CLIENT_BUFFER_SIZE / 2;
            ll_client->pop(this);
            ll_state = -4;
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
    if(client->next) {
      /* Client is already queued to write data. */
    } else {
      for(drain(); vpacket_pos == vpacket_size; drain()) {
        /* Create a virtual packet. */
        size_t amt = min(MUXSTREAM_CLIENT_BUFFER_SIZE - client->unacked_bytes,
                         min(count, MUXSTREAM_MAX_VPACKET));
        if(amt == 0) {
          break;
        }

        vpacket_pos = 0;
        vpacket_size = amt + 4;
        reinterpret_cast<uint16_t*>(vpacket)[0] = htons(client->client_id);
        reinterpret_cast<uint16_t*>(vpacket)[1] = htons(amt);
        memcpy(vpacket + 4, data, amt);

        data += amt; count -= amt;
        client->unacked_bytes += amt;

        if(write_head) {
          /* Other clients are waiting to transmit. */
          break;
        }
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

void MuxStream::wants_disconnect(MuxedClient* client) {
  assert(client->client_stream == NULL);
  wants_write(client);
  pop(lower_link);
}

bool MuxStream::pop(Stream* source) {
  if(source == lower_link) {
    for(drain(); write_head && vpacket_pos == vpacket_size; drain()) {
      MuxedClient* client = write_head;
      write_head = client->next == client ? NULL : client->next;
      client->next = NULL;

      if(client->received_bytes >= MUXSTREAM_CLIENT_BUFFER_SIZE / 2) {
        /* Send off an ACK for this client if we have enough data to ack. */
        client->received_bytes -= MUXSTREAM_CLIENT_BUFFER_SIZE / 2;

        vpacket_pos = 0;
        vpacket_size = 4;
        reinterpret_cast<uint16_t*>(vpacket)[0] = htons(client->client_id);
        reinterpret_cast<uint16_t*>(vpacket)[1] = htons(0);
        wants_write(client);
      } else if(client->client_stream) {
        /* Otherwise allow the client to send data. */
        client->pop(this);
      } else {
        /* Send a disconnect and delete the stream.  Since only one endpoint
         * is assnging new ids there is no possibility for id confusion. */
        bool need_delete = client->client_id & DELETE_ID_BIT;
        client->client_id |= DELETE_ID_BIT;

        vpacket_pos = 0;
        vpacket_size = 4;
        reinterpret_cast<uint16_t*>(vpacket)[0] = htons(client->client_id);
        reinterpret_cast<uint16_t*>(vpacket)[1] = htons(0);

        if(need_delete) {
          clients[client->client_id & ~DELETE_ID_BIT] = NULL;
          delete client;
        }
      }
    }
    return vpacket_pos < vpacket_size;
  } else {
    assert(0 && "muxedclient's should not call to pop");
    return false;
  }
}

void MuxStream::drain() {
  while(vpacket_pos < vpacket_size) {
    size_t amt = lower_link->push(this, vpacket + vpacket_pos,
                                  vpacket_size - vpacket_pos);
    if(amt == 0) {
      break;
    }
    vpacket_pos += amt;
  }
}

void MuxStream::attach_client(Stream* client_stream) {
  MuxedClient* client = new MuxedClient(this, client_stream);
  client_stream->attach_stream(client);

  for(size_t i = 0; i < MAX_CLIENTS; i++) {
    if(!clients[i]) {
      clients[i] = client;
      client->client_id = i;
      return;
    }
  }
  assert(0 && "todo: too many clients");
}

void MuxStream::replace_stream(Stream* old_link, Stream* new_link) {
  lower_link = new_link;
}

MuxedClient* MuxStream::get_stream(uint16_t id) {
  assert(id < MAX_CLIENTS);
  if(connector && !clients[id]) {
    Stream* client_stream = connector->connect();
    MuxedClient* client = new MuxedClient(this, client_stream);
    client_stream->attach_stream(client);
    client->client_id = id;
    clients[id] = client;

    DPRINTF("created new client %p:%u\n", client, id);
  }
  assert(clients[id] != NULL);
  return clients[id];
}

MuxedClient::MuxedClient(MuxStream* mux_stream, Stream* client_stream)
    : mux_stream(mux_stream), client_stream(client_stream), next(NULL),
      unacked_bytes(0), received_bytes(0), buf_pos(0), buf_size(0) {
}

MuxedClient::~MuxedClient() {
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
    return client_stream ? client_stream->pop(this) : false;
  }

  /* There is no reason to up-call to mux_stream to pop data.  Any data
   * mux_stream might have availble will be pushed through immediately because
   * the client streams are required to have enough buffer space to absorb all
   * data thrown at them. */
  drain();

  return buf_size != 0;
}

void MuxedClient::disconnect_stream(Stream* stream) {
  assert(stream == client_stream);
  client_stream = NULL;
  mux_stream->wants_disconnect(this);
}

void MuxedClient::drain() {
  for(; client_stream && buf_size > 0; ) {
    size_t wamt = min(buf_size, MUXSTREAM_CLIENT_BUFFER_SIZE - buf_pos);
    size_t amt = client_stream->push(this, buf + buf_pos, wamt);
    buf_pos = (buf_pos + amt) & (MUXSTREAM_CLIENT_BUFFER_SIZE - 1);
    buf_size -= amt;
    received_bytes += amt;
    if(amt != wamt) {
      break;
    }
  }
}
