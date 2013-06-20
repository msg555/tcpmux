#ifndef TCPMUX_STREAMFACTORY_H
#define TCPMUX_STREAMFACTORY_H

class Stream;

class StreamFactory {
 public:
  StreamFactory() {}
  virtual ~StreamFactory() {}

  virtual Stream* create(Stream* lower_link) = 0;
};

#endif
