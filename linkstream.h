#ifndef TCPMUX_LINKSTREAM_H
#define TCPMUX_LINKSTREAM_H

#include "stream.h"

#include <stddef.h>
#include <stdint.h>

#include <set>

const size_t LINKSTREAM_MAX_UNACKED = 1UL << 20;
const size_t LINKSTREAM_ACK_THRESH = 1UL << 16;
const size_t LINKSTREAM_KEY_SIZE = 64UL;
const size_t LINKSTREAM_MAX_VPACKET = 1UL << 12;

class MuxContext;
class StreamFactory;
class CompareStreams;

class LinkStream : public Stream {
  friend class CompareStreams;

 public:
  LinkStream(MuxContext* ctx, Stream* lower_link);
  LinkStream(MuxContext* ctx, Stream* lower_link, StreamFactory* factory);
  virtual ~LinkStream();

  virtual void set_forward(Stream* forward);

  virtual size_t push(Stream* source, const char* buf, size_t count);
  virtual bool pop(Stream* source);

  bool operator<(const LinkStream* x) const;

 private:
  void drain();

  MuxContext* ctx;
  Stream* lower_link;
  StreamFactory* factory;
  Stream* forward;

  size_t unacked_bytes;
  uint32_t read_bytes;

  size_t key_pos;
  char key[LINKSTREAM_KEY_SIZE];

  int link_read_state;
  size_t vpacket_len;
  uint32_t drop_bytes;

  size_t buf_pos;
  size_t buf_size;
  size_t acked_pos;
  char buf[LINKSTREAM_MAX_UNACKED];

  size_t out_pos;
  char out[LINKSTREAM_MAX_VPACKET + 2];

  static unsigned int key_seed;
  static std::set<LinkStream*, CompareStreams> streams;
};

#endif
