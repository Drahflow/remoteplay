#include "common.h"

#define __USE_BSD
#define __USE_POSIX199309
#define __USE_MISC
#define _POSIX_C_SOURCE

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
snd_output_t *output = NULL;
unsigned int periodSize;

char *alsaDevice = "hw:0,0";

int running;
float sampleRate = 44000;
float localPositionAvg;
float localPositionBlend = 0.0002;
int32_t samplesTooMuch = 0;
uint64_t maximumDrift = 4;

double targetLatency = 0.05;  // in s
uint64_t senderOffset; // incoming packet offset which would start at audioBuffer[0]
char audioBuffer[32000];

char receiveBuffer[8000];
uint64_t receivePos = 0;
int debugRate = 256;
int debugCounter = 0;

int beepOnFailure = 0;

static int set_hwparams(snd_pcm_t *handle,
            snd_pcm_hw_params_t *params,
            snd_pcm_access_t access)
{
    unsigned int channels = 2;
    unsigned int rate = 44100;
    unsigned int format = SND_PCM_FORMAT_S16_LE;
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

    unsigned int buffer_time = 5000;
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
    fprintf(stderr, "Buffer size is %lu\n", size);

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
    fprintf(stderr, "Period size: %lu\n", size);
    periodSize = size;
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
    err = snd_pcm_sw_params_set_start_threshold(handle, swparams, periodSize * 2);
    if (err < 0) {
        printf("Unable to set start threshold mode for playback: %s\n", snd_strerror(err));
        return err;
    }
    /* allow the transfer when at least period_size samples can be processed */
    /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
    err = snd_pcm_sw_params_set_avail_min(handle, swparams, periodSize);
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
    int64_t desiredLocalPosition = 4 * sampleRate * targetLatency;

    if(packetToPlayIn < 0) {
      fprintf(stderr, "Packet arrived too late.\n");
    } else if(localPosition < 0) {
      fprintf(stderr, "Playback is too far ahead.\n");

      failureSound(audioBuffer, sizeof(audioBuffer));
      senderOffset = packet->position - frameAlign(desiredLocalPosition);
      localPositionAvg = localPosition = packet->position - senderOffset;
    } else if(localPosition + dataLen > (int)sizeof(audioBuffer)) {
      fprintf(stderr, "Playback is too far behind.\n");

      failureSound(audioBuffer, sizeof(audioBuffer));
      senderOffset = packet->position - frameAlign(desiredLocalPosition);
      localPositionAvg = localPosition = packet->position - senderOffset;
    } else {
      memcpy(audioBuffer + localPosition, packet->data, dataLen);

      localPositionAvg = (1 - localPositionBlend) * localPositionAvg + localPositionBlend * localPosition;
    }

    if(++debugCounter > debugRate) {
      fprintf(stderr, "Packet for: +%lfs, buf pos: %lld, avg %f, delta %d\n", packetToPlayIn, (long long int)localPosition, localPositionAvg, samplesTooMuch);
      debugCounter = 0;
    }

    uint64_t shift = packet->length;
    memmove(receiveBuffer, receiveBuffer + shift, receivePos - shift);
    receivePos -= shift;

    if(localPositionAvg > desiredLocalPosition + maximumDrift) {
      samplesTooMuch = frameAlign(localPositionAvg - desiredLocalPosition);
      localPositionAvg = (0.1 * desiredLocalPosition + 0.9 * localPositionAvg);
    } else if(localPositionAvg < desiredLocalPosition - maximumDrift) {
      samplesTooMuch = frameAlign(localPositionAvg - desiredLocalPosition);
      localPositionAvg = (0.1 * desiredLocalPosition + 0.9 * localPositionAvg);
    }
  }
}

void writeAudio() {
  int requested = periodSize * 4;

  int err = snd_pcm_writei(handle, audioBuffer, periodSize);
  if(err == -EAGAIN) return;
  if(err < 0) {
      fprintf(stderr, "Err: %s\n", snd_strerror(err));
      if(xrun_recovery(handle, err) < 0) {
          printf("Write error: %s\n", snd_strerror(err));
          exit(EXIT_FAILURE);
      }
      return;
  }

  if(samplesTooMuch > 1) {
    requested += samplesTooMuch;
    samplesTooMuch = 0;
  } else if(samplesTooMuch < -1) {
    requested += samplesTooMuch;
    samplesTooMuch = 0;
  }

  if(requested < 0) requested = 0;
  if(requested > (int)sizeof(audioBuffer) / 4) requested = sizeof(audioBuffer) / 4;

  memmove(audioBuffer, audioBuffer + requested, sizeof(audioBuffer) - requested);
  failureSound(audioBuffer + sizeof(audioBuffer) - requested, requested);
  senderOffset += requested;
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

  err = snd_output_stdio_attach(&output, stdout, 0);
  if (err < 0) {
      printf("Output failed: %s\n", snd_strerror(err));
      return 0;
  }
  snd_pcm_dump(handle, output);
  if(fcntl(0, F_SETFL, O_NONBLOCK)) {
    fprintf(stderr, "Could not enable non-blocking mode for stdin: %s\n", strerror(errno));
    return 1;
  }

  running = 1;

  while(running) {
    writeAudio();
    receiveInput();

    usleep(1);
  }

  snd_pcm_close(handle);

  return 0;
}
