#include "common.h"

#define __USE_BSD

#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <alloca.h>
#include <alsa/asoundlib.h>

#define MIN_WRITE_SIZE 200
#define IGN(x) __##x __attribute__((unused))

snd_pcm_t *handle;
snd_pcm_hw_params_t *hwparams;
snd_pcm_sw_params_t *swparams;
snd_pcm_channel_area_t *areas;

char *alsaDevice = "default";

int running;
float initialSampleRate = 44100;
float sampleRate;
float sampleRateBlend = 0.0005;
float localPositionAvg;
float localPositionBlend = 0.05;

double targetLatency = 0.05;  // in s
uint64_t senderOffset; // incoming packet offset which would start at audioBuffer[0]
char audioBuffer[8000];

char receiveBuffer[8000];
uint64_t receivePos = 0;

int streamReady = 0;
int beepOnFailure = 0;

static int set_hwparams(snd_pcm_t *handle,
            snd_pcm_hw_params_t *params,
            snd_pcm_access_t access)
{
    unsigned int channels = 2;
    unsigned int rate = 44100;
    unsigned int format = SND_PCM_FORMAT_S16;
    unsigned int resample = 0;

    snd_pcm_uframes_t size;
    int err, dir;
    /* choose all parameters */
    err = snd_pcm_hw_params_any(handle, params);
    if (err < 0) {
        printf("Broken configuration for playback: no configurations available: %s\n", snd_strerror(err));
        return err;
    }
    /* set hardware resampling */
    err = snd_pcm_hw_params_set_rate_resample(handle, params, resample);
    if (err < 0) {
        printf("Resampling setup failed for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* set the interleaved read/write format */
    err = snd_pcm_hw_params_set_access(handle, params, access);
    if (err < 0) {
        printf("Access type not available for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* set the sample format */
    err = snd_pcm_hw_params_set_format(handle, params, format);
    if (err < 0) {
        printf("Sample format not available for playback: %s\n", snd_strerror(err));
        return err;
    }
    
    /* set the count of channels */
    err = snd_pcm_hw_params_set_channels(handle, params, channels);
    if (err < 0) {
        printf("Channels count (%u) not available for playbacks: %s\n", channels, snd_strerror(err));
        return err;
    }
    /* set the stream rate */
    unsigned int rrate = rate;
    err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
    if (err < 0) {
        printf("Rate %uHz not available for playback: %s\n", rate, snd_strerror(err));
        return err;
    }
    if (rrate != rate) {
        printf("Rate doesn't match (requested %uHz, get %iHz)\n", rate, err);
        return -EINVAL;
    }

    unsigned int buffer_time = 500;
    /* set the buffer time */
    err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &buffer_time, &dir);
    if (err < 0) {
        printf("Unable to set buffer time %u for playback: %s\n", buffer_time, snd_strerror(err));
        return err;
    }
    err = snd_pcm_hw_params_get_buffer_size(params, &size);
    if (err < 0) {
        printf("Unable to get buffer size for playback: %s\n", snd_strerror(err));
        return err;
    }

    unsigned int period_time = 500;
    /* set the period time */
    err = snd_pcm_hw_params_set_period_time_near(handle, params, &period_time, &dir);
    if (err < 0) {
        printf("Unable to set period time %u for playback: %s\n", period_time, snd_strerror(err));
        return err;
    }
    err = snd_pcm_hw_params_get_period_size(params, &size, &dir);
    if (err < 0) {
        printf("Unable to get period size for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* write the parameters to device */
    err = snd_pcm_hw_params(handle, params);
    if (err < 0) {
        printf("Unable to set hw params for playback: %s\n", snd_strerror(err));
        return err;
    }
    return 0;
}
static int set_swparams(snd_pcm_t *handle, snd_pcm_sw_params_t *swparams)
{
    int err;
    /* get the current swparams */
    err = snd_pcm_sw_params_current(handle, swparams);
    if (err < 0) {
        printf("Unable to determine current swparams for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* start the transfer when the buffer is almost full: */
    /* (buffer_size / avail_min) * avail_min */
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, MIN_WRITE_SIZE * 2);
    if (err < 0) {
        printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* allow the transfer when at least period_size samples can be processed */
    /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, MIN_WRITE_SIZE);
    if (err < 0) {
        printf("Unable to set avail min for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* write the parameters to the playback device */
    err = snd_pcm_sw_params(handle, swparams);
    if (err < 0) {
        printf("Unable to set sw params for playback: %s\n", snd_strerror(err));
        return err;
    }
    return 0;
}
/*
 *   Underrun and suspend recovery
 */
 
static int xrun_recovery(snd_pcm_t *handle, int err)
{
    printf("stream recovery\n");
    if (err == -EPIPE) {    /* under-run */
        err = snd_pcm_prepare(handle);
        if (err < 0)
            printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
        return 0;
    } else if (err == -ESTRPIPE) {
        while ((err = snd_pcm_resume(handle)) == -EAGAIN)
            sleep(1);   /* wait until the suspend flag is released */
        if (err < 0) {
            err = snd_pcm_prepare(handle);
            if (err < 0)
                printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
        }
        return 0;
    }
    return err;
}
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
      senderOffset = packet->position - sizeof(audioBuffer) / sampleRate * targetLatency;
      localPositionAvg = localPosition = packet->position - senderOffset;
      sampleRate = initialSampleRate;
    } else if(localPosition + dataLen > (int)sizeof(audioBuffer)) {
      fprintf(stderr, "Playback is too far behind.\n");

      failureSound(audioBuffer, sizeof(audioBuffer));
      senderOffset = packet->position - sizeof(audioBuffer) / sampleRate * targetLatency;
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

    if(sampleRate > 7500 && sampleRate < 8500) {
      if(streamReady) {
        fprintf(stderr, " -> new sample rate: %d", (uint32_t)sampleRate);
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
  int requested = 128;
  int err = 0;

  int len = snd_pcm_writei(handle, audioBuffer, requested);
  if(len == -EAGAIN) return;
  if(len < 0) {
      if(xrun_recovery(handle, err) < 0) {
          printf("Write error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
      }
      return;
  }

  memmove(audioBuffer, audioBuffer + requested * 4, sizeof(audioBuffer) - requested * 4);
  failureSound(audioBuffer + sizeof(audioBuffer) - requested, requested);
  senderOffset += requested * 4;

  // printf("Played %lld samples.\n", (long long int)requested);
}

int main(int argc, char **argv) {
  int err;

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

  snd_pcm_hw_params_alloca(&hwparams);
  snd_pcm_sw_params_alloca(&swparams);

  if ((err = snd_pcm_open(&handle, alsaDevice, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
      fprintf(stderr, "Playback open error: %s\n", snd_strerror(err));
      return 1;
  }

  if ((err = set_hwparams(handle, hwparams, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
      printf("Setting of hwparams failed: %s\n", snd_strerror(err));
      exit(EXIT_FAILURE);
  }
  if ((err = set_swparams(handle, swparams)) < 0) {
      printf("Setting of swparams failed: %s\n", snd_strerror(err));
      exit(EXIT_FAILURE);
  }

  if(fcntl(0, F_SETFL, O_NONBLOCK)) {
    fprintf(stderr, "Could not enable non-blocking mode for stdin: %s\n", strerror(errno));
    return 1;
  }

  running = 1;

  while(running) {
    writeAudio();
    receiveInput();

    usleep(50);
  }

  snd_pcm_close(handle);

  return 0;
}
