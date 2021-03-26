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
#include <math.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BUFFER_SIZE 400
#define IGN(x) __##x __attribute__((unused))
#define SAMPLE_RATE 44100

int running;

pa_context *play, *record;
pa_stream *playStream, *recordStream;
int playReady = 0;
// One second, two channels, with duplicate for module arithmetic
short testTone[SAMPLE_RATE * 4];
short recordBuffer[SAMPLE_RATE * 8];
size_t playPosition = 0;
size_t recordPosition = sizeof(recordBuffer) / sizeof(*recordBuffer);

uint64_t restartTime = 0;

void streamStateChanged(pa_stream *stream, void *IGN(userdata)) {
  pa_stream_state_t state = pa_stream_get_state(stream);
  fprintf(stderr, "pulseaudio stream state changed: %d\n", state);

  if(state != PA_STREAM_READY) return;

  if(stream == playStream) playReady = 1;
}

void dataAvailable(pa_stream *stream, size_t IGN(bytes), void *IGN(userdata)) {
  size_t available;
  const void *data;

  if(pa_stream_peek(stream, &data, &available)) {
    fprintf(stderr, "Failed to read data from stream: %s\n", pa_strerror(pa_context_errno(record)));
    return;
  }

  struct timespec t;
  if(clock_gettime(CLOCK_REALTIME, &t)) {
    fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
    return;
  }

  if(recordPosition + available / sizeof(*recordBuffer) < sizeof(recordBuffer) / sizeof(*recordBuffer)) {
    memcpy(recordBuffer + recordPosition, data, available);
    recordPosition += available / sizeof(*recordBuffer);
  }

  if(pa_stream_drop(stream)) {
    fprintf(stderr, "Failed to acknowledge stream data: %s\n", pa_strerror(pa_context_errno(record)));
    return;
  }
}

void analyzeRecording() {
  printf("---------------------------------\n");
  for(size_t off = 0; off < SAMPLE_RATE; ++off) {
    int64_t sum = 0;
    for(size_t i = 0; i < SAMPLE_RATE * 2; ++i) {
      sum += ((int64_t)testTone[i] * recordBuffer[i + off * 2]);
    }

    int64_t abssum = sum;
    if(abssum < 0) abssum = -abssum;

    if(abssum > (int64_t)SAMPLE_RATE * 2 * 500 * 500) {
      printf("%2.5f: %ld\n", (float)off / SAMPLE_RATE, sum);
    }
  }

  recordPosition = 0;
}

void contextStateChanged(pa_context *ctx, void *IGN(userdata)) {
  pa_context_state_t state = pa_context_get_state(ctx);
  fprintf(stderr, "pulseaudio context state changed: %d\n", state);

  if(state != PA_CONTEXT_READY) return;

  if(ctx == record) {
    pa_sample_spec sample_spec;
    sample_spec.format = PA_SAMPLE_S16LE;
    sample_spec.channels = 2;
    sample_spec.rate = SAMPLE_RATE;

    recordStream = pa_stream_new(ctx, "receiving test tones", &sample_spec, NULL);
    if(!recordStream) {
      fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(record)));
      running = 0;
      return;
    }

    pa_stream_set_state_callback(recordStream, streamStateChanged, NULL);
    pa_stream_set_read_callback(recordStream, dataAvailable, NULL);

    pa_buffer_attr buffer_spec;
    buffer_spec.maxlength = BUFFER_SIZE;
    buffer_spec.fragsize = BUFFER_SIZE;
    
    if(pa_stream_connect_record(recordStream, NULL, &buffer_spec, PA_STREAM_RECORD | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE)) {
      fprintf(stderr, "Failed to connect recording stream: %s\n", pa_strerror(pa_context_errno(record)));
      running = 0;
      return;
    }
  } else if(ctx == play) {
    pa_sample_spec sample_spec;
    sample_spec.format = PA_SAMPLE_S16LE;
    sample_spec.channels = 2;
    sample_spec.rate = SAMPLE_RATE;

    playStream = pa_stream_new(play, "playing test tones", &sample_spec, NULL);
    if(!playStream) {
      fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(play)));
      running = 0;
      return;
    }

    pa_stream_set_state_callback(playStream, streamStateChanged, NULL);
    // pa_stream_set_write_callback(stream, writeRequested, NULL);

    pa_buffer_attr buffer_spec;
    buffer_spec.maxlength = ~0u;
    buffer_spec.tlength = BUFFER_SIZE;
    buffer_spec.prebuf = ~0u;
    buffer_spec.minreq = ~0u;
    
    if(pa_stream_connect_playback(playStream, NULL, &buffer_spec, PA_STREAM_PLAYBACK | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE, NULL, NULL)) {
      fprintf(stderr, "Failed to connect playback stream: %s\n", pa_strerror(pa_context_errno(play)));
      running = 0;
      return;
    }
  } else {
    fprintf(stderr, "Unknown pulse stream encountered.\n");
  }
}

uint64_t now() {
  struct timespec t;
  if(clock_gettime(CLOCK_REALTIME, &t)) {
    fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
  }

  return (uint64_t)(t.tv_sec) * 1000000000 + t.tv_nsec;
}

void writeAudio() {
  if(!playReady) return;

  size_t requested = pa_stream_writable_size(playStream);
  if(!requested) return;

  if(pa_stream_is_corked(playStream)) {
    pa_stream_cork(playStream, 0, NULL, NULL);
  }
    
  if(pa_stream_write(playStream, testTone + playPosition, requested, NULL, 0, PA_SEEK_RELATIVE)) {
    fprintf(stderr, "Could not write to pulseaudio stream: %s\n", pa_strerror(pa_context_errno(play)));
    return;
  }

  playPosition += requested / sizeof(*testTone);
  if(playPosition > sizeof(testTone) / sizeof(*testTone) / 2) {
    analyzeRecording();
    bzero(recordBuffer, sizeof(recordBuffer));
    recordPosition = 0;

    playPosition -= sizeof(testTone) / sizeof(*testTone) / 2;
  }
}

int main(int IGN(argc), char **IGN(argv)) {
  pa_mainloop *mainloop = pa_mainloop_new();
  if(!mainloop) {
    fprintf(stderr, "Failed to get pulseaudio mainloop.\n");
    return 1;
  }

  play = pa_context_new(
    pa_mainloop_get_api(mainloop),
    "Calibrator: Play"
  );
  if(!play) {
    fprintf(stderr, "Failed to get pulseaudio context.\n");
    return 1;
  }

  pa_context_set_state_callback(play, contextStateChanged, NULL);

  if(pa_context_connect(play, NULL, PA_CONTEXT_NOFLAGS, NULL)) {
    fprintf(stderr, "Failed to connect pulseaudio context: %s\n", pa_strerror(pa_context_errno(play))); 
    return 1;
  }

  record = pa_context_new(
    pa_mainloop_get_api(mainloop),
    "Calibrator: Record"
  );
  if(!record) {
    fprintf(stderr, "Failed to get pulseaudio context.\n");
    return 1;
  }

  pa_context_set_state_callback(record, contextStateChanged, NULL);

  if(pa_context_connect(record, NULL, PA_CONTEXT_NOFLAGS, NULL)) {
    fprintf(stderr, "Failed to connect pulseaudio context: %s\n", pa_strerror(pa_context_errno(record))); 
    return 1;
  }

  for(size_t i = 0; i < sizeof(testTone) / sizeof(*testTone); ++i) {
    size_t channel = i % 2;
    double t = (double)(i / 2) / SAMPLE_RATE;

    if(channel == 0) {
      testTone[i] =
        sin(t * ((int)((t + 0.5) * 23555) % 7000)) * 8000 +
        sin(t * ((int)((t + 0.5) * 17379) % 9000)) * 8000 +
        sin(t * ((int)((t + 0.5) * 555333) % 9000)) * 8000 +
        sin(t * ((int)(t * 995737) % 10000)) * 8000;
    } else {
      testTone[i] = 0;
    }
  }
  bzero(testTone, SAMPLE_RATE);
  // memcpy(testTone + 2 * SAMPLE_RATE, testTone, sizeof(*testTone) * 2 * SAMPLE_RATE);

  running = 1;

  while(running) {
    pa_mainloop_iterate(mainloop, 0, NULL);
    writeAudio();

    usleep(50);
  }

  return 0;
}
