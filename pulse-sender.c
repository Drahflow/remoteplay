#include "common.h"

#define __USE_BSD
#define __USE_POSIX199309
#define __USE_XOPEN_EXTENDED

#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <netinet/ip.h>
#include <unistd.h>

#define BUFFER_SIZE 400
#define IGN(x) __##x __attribute__((unused))

int running;

pa_context *ctx;
pa_stream *stream;
uint64_t position;

char *pulseaudioName = "unnamed";

void streamStateChanged(pa_stream *IGN(stream), void *IGN(userdata)) {
  pa_stream_state_t state = pa_context_get_state(ctx);
  fprintf(stderr, "pulseaudio stream state changed: %d\n", state);
}

void dataAvailable(pa_stream *IGN(stream), size_t IGN(bytes), void *IGN(userdata)) {
  size_t available;
  const void *data;

  if(pa_stream_peek(stream, &data, &available)) {
    fprintf(stderr, "Failed to read data from stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }

  struct timespec t;
  if(clock_gettime(CLOCK_REALTIME, &t)) {
    fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
    return;
  }

  dataPacket packet;
  packet.length = available + 3 * sizeof(uint64_t);
  packet.position = position;
  packet.time = (uint64_t)(t.tv_sec) * 1000000000 + t.tv_nsec;
  memcpy(packet.data, data, available);

  write(1, &packet, sizeof(packet) - sizeof(packet.data) + available);

  position += available;
  // fprintf(stderr, "Data transmitted. Position now at: %llu\n", (long long unsigned int)position);

  if(pa_stream_drop(stream)) {
    fprintf(stderr, "Failed to acknowledge stream data: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }
}

void contextStateChanged(pa_context *IGN(ctx), void *IGN(userdata)) {
  pa_context_state_t state = pa_context_get_state(ctx);
  fprintf(stderr, "pulseaudio context state changed: %d\n", state);

  if(state != PA_CONTEXT_READY) return;

  pa_sample_spec sample_spec;
  sample_spec.format = PA_SAMPLE_S16LE;
  sample_spec.channels = 2;
  sample_spec.rate = 44100;

  stream = pa_stream_new(ctx, "forwarding", &sample_spec, NULL);
  if(!stream) {
    fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }

  pa_stream_set_state_callback(stream, streamStateChanged, NULL);
  pa_stream_set_read_callback(stream, dataAvailable, NULL);

  pa_buffer_attr buffer_spec;
  buffer_spec.maxlength = BUFFER_SIZE;
  buffer_spec.fragsize = BUFFER_SIZE;
  
  if(pa_stream_connect_record(stream, NULL, &buffer_spec, PA_STREAM_RECORD | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE)) {
    fprintf(stderr, "Failed to connect recording stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }
}

int main(int argc, char **argv) {
  if(argc != 1 && argc != 2) {
    fprintf(stderr, "Usage: ./pulse-sender [name]\n");
    return 1;
  }

  if(argc == 2) {
    pulseaudioName = argv[1];
  }

  position = 0;

  pa_mainloop *mainloop = pa_mainloop_new();
  if(!mainloop) {
    fprintf(stderr, "Failed to get pulseaudio mainloop.\n");
    return 1;
  }

  char nameBuf[1024];
  snprintf(nameBuf, 1024, "Remoteplay to %s", pulseaudioName);
  nameBuf[sizeof(nameBuf) - 1] = '\0';

  ctx = pa_context_new(
    pa_mainloop_get_api(mainloop),
    nameBuf
  );
  if(!ctx) {
    fprintf(stderr, "Failed to get pulseaudio context.\n");
    return 1;
  }

  pa_context_set_state_callback(ctx, contextStateChanged, NULL);

  if(pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL)) {
    fprintf(stderr, "Failed to connect pulseaudio context: %s\n", pa_strerror(pa_context_errno(ctx))); 
    return 1;
  }

  running = 1;

  while(running) {
    pa_mainloop_iterate(mainloop, 1, NULL);
  }

  return 0;
}
