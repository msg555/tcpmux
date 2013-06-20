#ifndef TCPMUX_STREAM_H
#define TCPMUX_STREAM_H

#include <assert.h>
#include <stddef.h>

class Stream {
 public:
  Stream() {}
  virtual ~Stream() {}

  /* Pushes data from the stream 'source' to this.  The stream should return
   * the number of bytes that were processed.  A push should never result
   * in a push back to source. */
  virtual size_t push(Stream* source, const char* buf, size_t count) = 0;

  /* Solicits the stream object to output any pending data to 'source'.
   * Returning true indicates that there is still more data to write. */
  virtual bool pop(Stream* source) = 0;

  virtual void attach_stream(Stream* stream) {
    assert(0 && "attach_stream not supported on stream");
  }

  virtual void replace_stream(Stream* old_stream, Stream* new_stream) {
    assert(0 && "replace_stream not supported on stream");
  }

  virtual void disconnect_stream(Stream* stream) {
  }
};

#endif
