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
#include <fcntl.h>
#include <unistd.h>

#define IGN(x) __##x __attribute__((unused))

int BUFFER_SIZE = 400;

int running;
float initialSampleRate = 44100;
float sampleRate;
float sampleRateBlend = 0.0005;
float localPositionAvg;
float localPositionBlend = 0.05;

double targetLatency = 0.05;  // in s
uint64_t senderOffset; // incoming packet offset which would start at audioBuffer[0]
char audioBuffer[120000];

char receiveBuffer[8000];
uint64_t receivePos = 0;

pa_context *ctx;
pa_stream *stream;

int streamReady = 0;
int beepOnFailure = 0;

// drop-in replacement for bzero on audioBuffer
void failureSound(char *bufferPos, size_t len) {
  char reference;
  if(bufferPos == audioBuffer) {
    reference = bufferPos[0];
  } else {
    reference = bufferPos[-1];
  }

  if(!beepOnFailure) {
    for(size_t i = 0; i < len; ++i) {
      bufferPos[i] = reference;
    }
  } else if(reference > 0) {
    for(size_t i = 0; i < len; ++i) {
      bufferPos[i] = reference - (i % 4? 0: 4);
    }
  } else {
    for(size_t i = 0; i < len; ++i) {
      bufferPos[i] = reference + (i % 4? 0: 4);
    }
  }
}

void streamStateChanged(pa_stream *IGN(stream), void *IGN(userdata)) {
  pa_stream_state_t state = pa_stream_get_state(stream);
  fprintf(stderr, "pulseaudio stream state changed: %d\n", state);

  if(state != PA_STREAM_READY) return;

  streamReady = 1;
}

void contextStateChanged(pa_context *IGN(ctx), void *IGN(userdata)) {
  pa_context_state_t state = pa_context_get_state(ctx);
  fprintf(stderr, "pulseaudio context state changed: %d\n", state);

  if(state != PA_CONTEXT_READY) return;

  pa_sample_spec sample_spec;
  sample_spec.format = PA_SAMPLE_S16LE;
  sample_spec.channels = 2;
  sample_spec.rate = sampleRate;

  stream = pa_stream_new(ctx, "remoteplay-receiver", &sample_spec, NULL);
  if(!stream) {
    fprintf(stderr, "Failed to create pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }

  pa_stream_set_state_callback(stream, streamStateChanged, NULL);
  // pa_stream_set_write_callback(stream, writeRequested, NULL);

  pa_buffer_attr buffer_spec;
  buffer_spec.maxlength = ~0u;
  buffer_spec.tlength = BUFFER_SIZE;
  buffer_spec.prebuf = ~0u;
  buffer_spec.minreq = ~0u;
  
  if(pa_stream_connect_playback(stream, NULL, &buffer_spec, PA_STREAM_PLAYBACK | PA_STREAM_ADJUST_LATENCY | PA_STREAM_NOT_MONOTONIC | PA_STREAM_VARIABLE_RATE, NULL, NULL)) {
    fprintf(stderr, "Failed to connect playback stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    running = 0;
    return;
  }
}

int frameAlign(float f) {
  return ((int)f) / 4 * 4;
}

void receiveInput() {
  while(1) {
    int len = read(0, receiveBuffer + receivePos, sizeof(receiveBuffer) - receivePos);
    if(len < 0) {
      if(errno == EAGAIN || errno == EWOULDBLOCK) return;

      fprintf(stderr, "Failed to receive packet: %s\n", strerror(errno));
      return;
    } else if(len == 0) {
      running = 0;
    } else {
      receivePos += len;
    }

    dataPacket *packet = (dataPacket *)receiveBuffer;
    if(receivePos < sizeof(packet->length) || receivePos < packet->length) continue;

    struct timespec t;
    if(clock_gettime(CLOCK_REALTIME, &t)) {
      fprintf(stderr, "Failed to get current time: %s\n", strerror(errno));
    }

    uint64_t now = (uint64_t)(t.tv_sec) * 1000000000 + t.tv_nsec;
    double packetToPlayIn = (packet->time + targetLatency * 1000000000 - now) / 1000000000;

    int dataLen = packet->length - sizeof(*packet) + sizeof(packet->data);
    int64_t localPosition = packet->position - senderOffset;

    fprintf(stderr, "Packet for: +%lfs, buf pos: %lld", packetToPlayIn, (long long int)localPosition);

    if(packetToPlayIn < 0) {
      fprintf(stderr, "Packet arrived too late.\n");
    } else if(localPosition < 0) {
      fprintf(stderr, "Playback is too far ahead.\n");

      failureSound(audioBuffer, sizeof(audioBuffer));
      senderOffset = packet->position - frameAlign(sampleRate * targetLatency);
      localPositionAvg = localPosition = packet->position - senderOffset;
      sampleRate = initialSampleRate;
    } else if(localPosition + dataLen > (int)sizeof(audioBuffer)) {
      fprintf(stderr, "Playback is too far behind.\n");

      failureSound(audioBuffer, sizeof(audioBuffer));
      senderOffset = packet->position - frameAlign(sampleRate * targetLatency);
      localPositionAvg = localPosition = packet->position - senderOffset;
      sampleRate = initialSampleRate;
    } else {
      memcpy(audioBuffer + localPosition, packet->data, dataLen);

      float onTargetSampleRate = localPosition / packetToPlayIn;

      localPositionAvg = (1 - localPositionBlend) * localPositionAvg + localPositionBlend * localPosition;

      fprintf(stderr, ", pkg sample rate %f", onTargetSampleRate);

      float newSampleRate = (1 - sampleRateBlend) * sampleRate + sampleRateBlend * onTargetSampleRate;

      if(newSampleRate < sampleRate && localPosition < localPositionAvg) {
        sampleRate = newSampleRate;
      } else if(newSampleRate > sampleRate && localPosition > localPositionAvg) {
        sampleRate = newSampleRate;
      }
    }

    if(sampleRate > 43000 && sampleRate < 45000) {
      if(streamReady) {
        fprintf(stderr, " -> new sample rate: %d", (uint32_t)sampleRate);
        pa_operation *op = pa_stream_update_sample_rate(stream, sampleRate, NULL, NULL);
        if(op) {
          pa_operation_unref(op);
        }
      } else {
        fprintf(stderr, " Stream is not ready, yet.");
      }
    }

    fprintf(stderr, "\n");

    uint64_t shift = packet->length;
    memmove(receiveBuffer, receiveBuffer + shift, receivePos - shift);
    receivePos -= shift;
  }
}

void writeAudio() {
  if(!streamReady) return;

  size_t requested = pa_stream_writable_size(stream);
  if(!requested) return;

  if(pa_stream_is_corked(stream)) {
    pa_stream_cork(stream, 0, NULL, NULL);
  }
    
  if(pa_stream_write(stream, audioBuffer, requested, NULL, 0, PA_SEEK_RELATIVE)) {
    fprintf(stderr, "Could not write to pulseaudio stream: %s\n", pa_strerror(pa_context_errno(ctx)));
    return;
  }

  memmove(audioBuffer, audioBuffer + requested, sizeof(audioBuffer) - requested);
  failureSound(audioBuffer + sizeof(audioBuffer) - requested, requested);
  senderOffset += requested;

  // printf("Played %lld samples.\n", (long long int)requested);
}

int main(int argc, char **argv) {
  if(argc != 2 && argc != 3) {
    fprintf(stderr, "Usage: ./pulse-receiver [target latency]\n");
    return 1;
  }

  if(argc == 2) {
    targetLatency = atof(argv[1]);
  }
  fprintf(stderr, "Target latency: %f\n", targetLatency);

  senderOffset = -1ull << 62;
  sampleRate = initialSampleRate;

  pa_mainloop *mainloop = pa_mainloop_new();
  if(!mainloop) {
    fprintf(stderr, "Failed to get pulseaudio mainloop.\n");
    return 1;
  }

  ctx = pa_context_new(
    pa_mainloop_get_api(mainloop),
    "Remoteplay"
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

  if(fcntl(0, F_SETFL, O_NONBLOCK)) {
    fprintf(stderr, "Could not enable non-blocking mode for stdin: %s\n", strerror(errno));
    return 1;
  }

  running = 1;

  while(running) {
    pa_mainloop_iterate(mainloop, 0, NULL);

    writeAudio();
    receiveInput();

    usleep(50);
  }

  return 0;
}
