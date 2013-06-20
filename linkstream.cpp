#include "linkstream.h"
#include "streamfactory.h"

#include <algorithm>

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arpa/inet.h>

using std::min;

struct CompareStreams {
  bool operator()(const LinkStream* x, const LinkStream* y) const {
    return memcmp(x->key, y->key, LINKSTREAM_KEY_SIZE) < 0;
  }
};

unsigned int LinkStream::key_seed = 0;

std::set<LinkStream*, CompareStreams> LinkStream::streams;

LinkStream::LinkStream(MuxContext* ctx, Stream* lower_link)
    : ctx(ctx), lower_link(lower_link), factory(NULL), forward(NULL),
      unacked_bytes(0), read_bytes(0),
      key_pos(0), link_read_state(-4), vpacket_len(0),
      buf_pos(0), buf_size(0), acked_pos(0) {
  if(key_seed == 0) {
    key_seed = time(NULL);
  }
  for(size_t i = 0; i < LINKSTREAM_KEY_SIZE; i++) {
    key[i] = rand(); //rand_r(&key_seed) & 0xFF;
  }
  key_pos = LINKSTREAM_KEY_SIZE;
  out_pos = sizeof(out) - LINKSTREAM_KEY_SIZE - 4;
  memcpy(out + out_pos, key, LINKSTREAM_KEY_SIZE);
  *reinterpret_cast<uint32_t*>(out + sizeof(out) - 4) = 0U;
}

LinkStream::LinkStream(MuxContext* ctx, Stream* lower_link,
                       StreamFactory* factory)
    : ctx(ctx), lower_link(lower_link), factory(factory), forward(NULL),
      unacked_bytes(0), read_bytes(0),
      key_pos(0), link_read_state(-4), vpacket_len(0),
      buf_pos(0), buf_size(0), acked_pos(0) {
  assert(factory != NULL);
  out_pos = sizeof(out);
}

LinkStream::~LinkStream() {
  if(lower_link) {
    lower_link->disconnect_stream(this);
  }
}

#include "tcpmux.h"

size_t LinkStream::push(Stream* source, const char* data, size_t count) {
  assert(source == lower_link || source == forward);

  const size_t initial_count = count;
  if(source == forward) {
    /* Copy data from the forward stream into our write buffer. */
    for(drain(); ; drain()) {
      size_t pos = (buf_pos + buf_size) & (LINKSTREAM_MAX_UNACKED - 1);

      size_t amt = min(count, LINKSTREAM_MAX_UNACKED - pos);
      amt = min(amt, LINKSTREAM_MAX_UNACKED - buf_size -
                     ((buf_pos - acked_pos) & (LINKSTREAM_MAX_UNACKED - 1)));
      if(amt == 0) {
        break;
      }

      memcpy(buf + pos, data, amt);
      buf_size += amt;
      data += amt; count -= amt;
    }
  } else for(; count != 0; ) {
    if(key_pos < LINKSTREAM_KEY_SIZE) {
      /* We are still receiving the key. */
      size_t amt = min(count, LINKSTREAM_KEY_SIZE - key_pos);
      memcpy(key + key_pos, data, amt);
      key_pos += amt;
      data += amt; count -= amt;

      /* Check if we need to link this stream to an existing stream. */
      if(key_pos == LINKSTREAM_KEY_SIZE) {
        std::set<LinkStream*, CompareStreams>::iterator it = streams.find(this);
        if(it != streams.end()) {
          /* Copy out information from the linked stream. */
          LinkStream* lnk = *it;
          forward = lnk->forward;
          forward->replace_stream(lnk, this);
          read_bytes = lnk->read_bytes;

          /* Copy the write state and rewind the write buffer to a known safe
           * position. */
          memcpy(buf, lnk->buf, LINKSTREAM_MAX_UNACKED);
          buf_pos = lnk->buf_pos;
          buf_size = lnk->buf_size;

          /* Cleanup! */
          streams.erase(it);
          delete lnk;
        } else {
          forward = factory->create(this);
        }

        /* Insert ourself into the streams linking set. */
        streams.insert(this);

        out_pos = sizeof(out) - 4;
        *reinterpret_cast<uint32_t*>(out + out_pos) = htonl(read_bytes);
      }
    } else if(link_read_state < 0) {
      /* Receive the remote's write position. */
      size_t amt = min(count, (size_t)-link_read_state);
      memcpy((char*)&acked_pos + 4 + link_read_state, data, amt);
      link_read_state += amt;
      data += amt; count -= amt;
      if(link_read_state == 0) {
        acked_pos = ntohl(acked_pos);
        buf_size = (buf_pos + buf_size - acked_pos) &
                    (LINKSTREAM_MAX_UNACKED - 1);
        buf_pos = acked_pos;
        drain();
      }
    } else if(link_read_state < 2) {
      size_t amt = min(count, (size_t)(2UL - link_read_state));
      memcpy((char*)&vpacket_len + link_read_state, data, amt);
      link_read_state += amt;
      data += amt; count -= amt;

      if(link_read_state == 2) {
        vpacket_len = ntohs(vpacket_len);
        if(vpacket_len == 0) {
          link_read_state = 0;
          acked_pos = (acked_pos + LINKSTREAM_ACK_THRESH) &
                            (LINKSTREAM_MAX_UNACKED - 1);
        }
      }
    } else {
      size_t wamt = min((size_t)vpacket_len, count);
      size_t amt = forward->push(this, data, wamt);
      read_bytes = (read_bytes + amt) & (LINKSTREAM_MAX_UNACKED - 1);
      unacked_bytes += amt;
      data += amt; count -= amt;
      vpacket_len -= amt;
      if(vpacket_len == 0) {
        link_read_state = 0;
      }
    }
  }
  return initial_count - count;
}

bool LinkStream::pop(Stream* source) {
  if(source == lower_link) {
    bool more = true;
    for(drain(); more && out_pos == sizeof(out); drain()) {
      more = forward ? forward->pop(this) : false;
    }
    return out_pos < sizeof(out);
  } else {
    return lower_link->pop(this);
  }
}

void LinkStream::disconnect_stream(Stream* stream) {
  assert(stream == lower_link);
  lower_link = NULL;
}

void LinkStream::attach_stream(Stream* forward) {
  assert(factory == NULL);
  this->forward = forward;
}

void LinkStream::replace_stream(Stream* old_stream, Stream* new_stream) {
  lower_link->disconnect_stream(this);
  lower_link = new_stream;
  lower_link->attach_stream(this);

  unacked_bytes = 0;
  link_read_state = -4;
  out_pos = sizeof(out) - LINKSTREAM_KEY_SIZE - 4;
  memcpy(out + out_pos, key, LINKSTREAM_KEY_SIZE);
  *reinterpret_cast<uint32_t*>(out + sizeof(out) - 4) = htonl(read_bytes);

  drain();
}

void LinkStream::drain() {
  for(;;) {
    if(out_pos == sizeof(out)) {
      if(unacked_bytes >= LINKSTREAM_ACK_THRESH) {
        /* Create an ack virtual packet. */
        unacked_bytes -= LINKSTREAM_ACK_THRESH;
        out_pos = sizeof(out) - 2;
        *reinterpret_cast<uint16_t*>(out + out_pos) = 0;
      } else if(link_read_state >= 0 && buf_size > 0) {
        size_t amt = min(min(buf_size, LINKSTREAM_MAX_UNACKED - buf_pos),
                         LINKSTREAM_MAX_VPACKET);
        out_pos = sizeof(out) - amt - 2;
        *reinterpret_cast<uint16_t*>(out + out_pos) = htons(amt);
        memcpy(out + out_pos + 2, buf + buf_pos, amt);
        buf_pos = (buf_pos + amt) & (LINKSTREAM_MAX_UNACKED - 1);
        buf_size -= amt;
      } else {
        break;
      }
    }

    size_t amt = lower_link->push(this, out + out_pos, sizeof(out) - out_pos);
    if(amt == 0) {
      break;
    }
    out_pos += amt;
  }
}
