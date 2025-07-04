#ifndef H_05803AA0_0287_4370_B6B9_E75FD1BBEF93
#define H_05803AA0_0287_4370_B6B9_E75FD1BBEF93

#include <stdint.h>

struct dataPacket_t {
  uint64_t length;
  uint64_t position;
  uint64_t time; // nanoseconds since the epoch
  char data[4096];
};

typedef struct dataPacket_t dataPacket;

#endif
