#include "vupeak.h"

/* Use the newer ALSA API */
#define ALSA_PCM_NEW_HW_PARAMS_API

#include <alsa/asoundlib.h>
#include <PCA9685.h>
#include <signal.h>
#include <fftw3.h>
#include <math.h>
#include <sys/time.h>
#include <unistd.h>
#include "config.h"

// globals, defined here only for intHandler() to cleanup
audiopwm args;
int fd;
char *buffer;
snd_pcm_t *handle;
// verbosity flag
bool verbose = false;


unsigned Microseconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec*1000000 + ts.tv_nsec/1000;
}


void intHandler(int dummy) {
  // turn off all channels
  PCA9685_setAllPWM(fd, args.pwm_addr, _PCA9685_MINVAL, _PCA9685_MINVAL);

  // cleanup alsa
  snd_pcm_drain(handle);
  snd_pcm_close(handle);
  //free(buffer);

  // cleanup fftw
  //fftw_destroy_plan(p);
  //fftw_free(in);
  //fftw_free(out);

  exit(dummy);
}

int initPCA9685(audiopwm args) {
  _PCA9685_DEBUG = args.pwm_debug;
  int afd = PCA9685_openI2C(args.pwm_bus, args.pwm_addr);
  PCA9685_initPWM(afd, args.pwm_addr, args.pwm_freq);
  return afd;
}


snd_pcm_t* initALSA(audiopwm *args, char **bufferPtr) {
  int rc;
  snd_pcm_t *handle;
  snd_pcm_hw_params_t *params;
  rc = snd_pcm_open(&handle, args->audio_device, SND_PCM_STREAM_CAPTURE, 0);
  if (rc < 0) {
    fprintf(stderr, "unable to open pcm device '%s': %s\n", args->audio_device, snd_strerror(rc));
    exit(1);
  }

  snd_pcm_hw_params_alloca(&params);
  rc = snd_pcm_hw_params_any(handle, params);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_any() failed %d\n", rc);
  rc = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_access() failed %d\n", rc);
  rc = snd_pcm_hw_params_set_format(handle, params, SND_PCM_FORMAT_S16_LE);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_format() failed %d\n", rc);
  rc = snd_pcm_hw_params_set_channels(handle, params, 2);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_channels() failed %d\n", rc);

  rc = snd_pcm_hw_params_set_rate_near(handle, params, &args->audio_rate, NULL);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_rate_near() failed %d\n", rc);
  fprintf(stdout, "sample rate %u\n", args->audio_rate);

  rc = snd_pcm_hw_params_set_period_size_near(handle, params, (snd_pcm_uframes_t *) &args->audio_period, NULL);
  if (rc < 0) fprintf(stderr, "snd_pcm_hw_params_set_period_size_near() failed %d\n", rc);
  fprintf(stdout, "audio period %d\n", args->audio_period);
  args->audio_buffer_period = args->audio_period;

  if (args->audio_overlap) args->audio_buffer_period *= 2;
  fprintf(stdout, "buffer period %d\n", args->audio_buffer_period);

  rc = snd_pcm_hw_params(handle, params);
  if (rc < 0) {
    fprintf(stderr, "unable to set hw parameters: (%d), %s\n", rc, snd_strerror(rc));
    exit(1);
  }
  args->audio_buffer_size = args->audio_buffer_period * 2 * args->audio_channels; /* 2 bytes/sample, 2 channels */
  fprintf(stdout, "buffer size %d\n", args->audio_buffer_size);
  *bufferPtr = (char *) malloc(args->audio_buffer_size);
  return handle;
}



double *hanning(int N) {
  int i;
  double *window = (double *) malloc(sizeof(double) * N);
  int M = N % 2 == 0 ? N / 2 : (N + 1) / 2;
  if (verbose) fprintf(stdout, "hanning %d %d\n", N, M);
  for (i = 0; i < M; i++) {
    window[i] = 0.5 * (1 - cos(2 * M_PI * i / (N - 1)));
    window[N - i - 1] = window[i];
    if (verbose) printf("%d: %f  %d: %f\n", i, window[i], N - i - 1, window[N - i - 1]);
  }
  if (verbose) {
    for (i = 0; i < N; i++) {
      fprintf(stdout, "%f ", window[i]);
    }
    fprintf(stdout, "\n");
  }
  return window;
}


// period p 1024, rate r 44100, bus b 1, address a 0x40, pwm freq f 200, audio device d default, mode m level (spectrum)
void process_args(int argc, char **argv) {
  char *usage = "\
Usage: vupeak [-m level|spectrum] [-d audio device] [-p audio period] [-r audio rate] [-c audio channels] [-o]\n\
              [-b pwm bus] [-a pwm address] [-f pwm frequency] [-v] [-D] [-s pwm smoothing] [-h]\n\
where\n\
  -m sets the mode of audio processing (spectrum)\n\
  -d sets the audio device (default)\n\
  -p sets the audio period (1024)\n\
  -r sets the audio rate (44100)\n\
  -c sets the audio channels (2)\n\
  -o sets the audio overlap to false (true)\n\
  -b sets the pwm i2c bus (1)\n\
  -a sets the pwm i2c address (0x40)\n\
  -f sets the pwm frequency (200)\n\
  -v sets verbose to true (false)\n\
  -D sets debug to true (false)\n\
  -s sets the pwm smoothing (1)\n\
  -h sets the fft hanning to true (false)\n";

  // default values
  args.mode = 2;
  args.audio_device = "default";
  args.audio_period = 1024;
  args.audio_rate = 44100;
  args.audio_channels = 2;
  args.audio_overlap = true;
  args.pwm_bus = 1;
  args.pwm_addr = 0x40;
  args.pwm_freq = 200;
  args.pwm_debug = false;
  args.pwm_smoothing = 1;
  args.fft_hanning = false;

  opterr = 0;
  int c;
  while ((c = getopt(argc, argv, "m:d:p:r:c:ob:a:f:vDs:h")) != -1) {
    switch (c) {
      case 'm':
        args.mode = 0;
        if (strcmp(optarg, "level") == 0) args.mode = 1;
        else if (strcmp(optarg, "spectrum") == 0) args.mode = 2;
        if (args.mode == 0) {
          fprintf(stderr, "Illegal mode %s\n", optarg);
          exit(-1);
        } // if
        break;
      case 'd':
        args.audio_device = optarg;
        break;
      case 'p':
        args.audio_period = atoi(optarg);
        break;
      case 'r':
        args.audio_rate = atoi(optarg);
        break;
      case 'c':
        args.audio_channels = atoi(optarg);
        break;
      case 'o':
        args.audio_overlap = false;
        break;
      case 'b':
        args.pwm_bus = atoi(optarg);
        break;
      case 'a':
        args.pwm_addr = atoi(optarg);
        break;
      case 'f':
        args.pwm_freq = atoi(optarg);
        break;
      case 'v':
        verbose = true;
        break;
      case 'D':
        args.pwm_debug = true;
        break;
      case 's':
        args.pwm_smoothing = atoi(optarg);
        break;
      case 'h':
        args.fft_hanning = true;
        break;
      case '?':
        fprintf(stdout, "%s", usage);
        exit(0);
      default:
        fprintf(stderr, "optopt '%c'\n", optopt);
        fprintf(stderr, "%s", usage);
        abort();
    } //switch
  } // while
  // sanity checks
  if (args.mode == 1) {
    fprintf(stdout, "'level' mode suggested args: -p 256 -r 192000\n");
    fprintf(stdout, "'level' mode suggested line level input volume at 100%%\n");
  } // if mode && period
  else if (args.mode == 2) {
    fprintf(stdout, "'spectrum' mode suggested cut line level input volume to 50%%\n");
  } // if mode && period
} // process_args


char *representation(float val) {
  if (val < 50) return " ";
  if (val < 52) return "-";
  if (val < 54) return ".";
  if (val < 56) return ",";
  if (val < 58) return ":";
  if (val < 60) return ";";
  if (val < 62) return "+";
  if (val < 64) return "=";
  if (val < 66) return "&";
  if (val < 68) return "@";
  return "# ";
}




int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  fprintf(stdout, "vupeak %d.%d\n", libPCA9685_VERSION_MAJOR, libPCA9685_VERSION_MINOR);
  process_args(argc, argv);

  signal(SIGINT, intHandler);

  // ALSA init
  handle = initALSA(&args, &buffer);

  // libPCA9685 init
  fd = initPCA9685(args);

  // fftw init
  // the buffer period may be bigger than the audio period
  int N = args.audio_buffer_period;
  printf("N %d\n", N);
  fftw_plan p;
  fftw_complex *out = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * N);
  double in[N];
  p = fftw_plan_dft_r2c_1d(N, in, out, FFTW_ESTIMATE);
  double *han;
  if (args.fft_hanning) han = hanning(N);

  int average = 0;
  int minValue = 32000;
  int maxValue = -32000;
  int rc = 64;
  rc = 1024;

  // bass 44100 1024:
  unsigned int bins[16] = {0,0,1, 0,2,2, 0,3,0, 4,4,0, 20,0,0};
  unsigned int binwidths[16] = {0,0,1, 0,1,1, 0,1,0, 16,16,0, 20,0,0};

  double mins[16] = { 100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100 };
  double maxs[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
  { int i;
    for (i = 0; i < 16; i++) {
      if (verbose) printf("%d %f - %f\n", i, mins[i], maxs[i]);
    }
  }
  //unsigned int bins[16] = {0,0,2, 0,3,3, 0,4,0, 5,5,0, 6,0,0};
  int loop = 0;
  unsigned int b = Microseconds();
  printf("loops  ms    hz\n");

  bool testperiod = false;

  FILE* infh;
  FILE* outfh;
  if (testperiod) infh = fopen("input.dat", "w");
  if (testperiod) outfh = fopen("output.dat", "w");
  while (1) {
    if (args.audio_overlap) {
      memcpy(buffer + args.audio_buffer_size / 2, buffer, args.audio_buffer_size / 2);
    } // overlap
    rc = snd_pcm_readi(handle, buffer, args.audio_period);
    if (rc == -EPIPE) {
      fprintf(stderr, "overrun occurred\n");
      snd_pcm_prepare(handle);
    } else if (rc < 0) {
      fprintf(stderr, "error from read: %s\n", snd_strerror(rc));
    } else if (rc != (int) args.audio_period) {
      fprintf(stderr, "short read, read %d frames\n", rc);
    } else {

      if (args.mode == 1) {
        // find the min and max value
        int sample;
        long tmp;
        int maxSample=-32000;
        int minSample=32000;

        // process all frames recorded
        unsigned int frame;
        for (frame = 0; frame < args.audio_buffer_period; frame++) {
          tmp = (long) ((char*) buffer)[2 * args.audio_channels * frame + 1] << 8 | ((char*) buffer)[2 * args.audio_channels * frame];
          if (tmp < 32768) sample = tmp;
          else sample = tmp - 65536;
          if (sample > maxSample) {
            maxSample = sample;
          } // if sample >
          if (sample < minSample) {
            minSample = sample;
          } // if sample <
        } // for frame

        // intensity-based value
        int intensity_value = maxSample - minSample;

        // minValue is the smallest value seen, use for offset
        if (intensity_value < minValue) {
          minValue = intensity_value;
        }
        if (intensity_value > maxValue) {
          maxValue = intensity_value;
        }
        intensity_value -= minValue;

        // modified moving average for smoothing
        int alpha = args.pwm_smoothing;
        average = (intensity_value + (alpha-1) * average) / alpha;

        int ratio = 100.0 * average / (maxValue - minValue);
        ratio = (ratio / 10.0) * (ratio / 10.0);
        ratio = (ratio / 10.0) * (ratio / 10.0);
        if (ratio < 0) ratio = 0;
        if (ratio > 100) ratio = 100;

        int display = ratio / 100.0 * _PCA9685_MAXVAL;
        if (verbose) fprintf(stdout, "%d %d\n", intensity_value, ratio);

        // update the pwms
        PCA9685_setAllPWM(fd, args.pwm_addr, 0, display);
      }

      else if (args.mode == 2) {
        // window for fftw
        unsigned int frame;
        long tmp;
        double sample;
        for (frame = 0; frame < args.audio_buffer_period; frame++) {
          tmp = (long) ((char*) buffer)[2 * args.audio_channels * frame + 1] << 8 | ((char*) buffer)[2 * args.audio_channels * frame];
          if (tmp < 32768) sample = tmp;
          else sample = tmp - 65536;
          // apply hanning window
          in[frame] = (double) sample;
          if (args.fft_hanning) in[frame] *= han[frame];
          if (testperiod) fprintf(infh, "%f\n", in[frame]);
        } // for frame

        // fftw
        fftw_execute(p);
        int j;
        for (j = 0; j < 512; j++) {
          float val = 20.0 * log10f(2.0 * sqrtf(out[j][0]*out[j][0] + out[j][1]*out[j][1]) / args.audio_buffer_period);
          //if (j <= 30) printf("%s", representation(val));
          if (testperiod) fprintf(outfh, "%f\n", val);
        } // for j
        //printf("\n");
        unsigned int pwmoff[16];
        int pwmindex;
        for (pwmindex = 0; pwmindex < 16; pwmindex++) {
            // normalize by the number of frames in a period and the hanning factor
            unsigned int binindex = bins[pwmindex];
            unsigned int width = binwidths[pwmindex];
            if (binindex == 0) {
              pwmoff[pwmindex] = 0;
              continue;
            } // if index
            unsigned int j;
            double amp = 0;
            int ampbinindex = binindex;
            for (j = 0; j < width; j++) {
              double mag = 2.0 * sqrtf(out[binindex + j][0] * out[binindex + j][0] + out[binindex + j][1] * out[binindex + j][1]) / args.audio_buffer_period;
              double thisamp = 20 * log10f(mag);
              //amp += thisamp;
              if (thisamp > amp) {
                amp = thisamp;
                ampbinindex = binindex + j;
              } // if thisamp
            } // for j
            //amp /= width;
            int a = 3;
            //if (verbose) printf("check: %d amp %f mins[%d] %f maxs[%d] %f\n", ampbinindex, amp, pwmindex, mins[pwmindex], pwmindex, maxs[pwmindex]);
            if (verbose) printf("check: amp %f mins[%d] %f maxs[%d] %f\n", amp, pwmindex, mins[pwmindex], pwmindex, maxs[pwmindex]);
            if (amp > maxs[pwmindex]) {
              if (verbose) printf("maxs[%d] %f amp %f newmax %f\n", pwmindex, maxs[pwmindex], amp, ((a - 1) * maxs[pwmindex] + amp) / a);
              maxs[pwmindex] = ((a - 1) * maxs[pwmindex] + amp) / a;
            }
            if (amp < mins[pwmindex]) {
              if (verbose) printf("mins[%d] %f amp %f newmin %f\n", pwmindex, mins[pwmindex], amp, ((a - 1) * mins[pwmindex] + amp) / a);
              mins[pwmindex] = ((a - 1) * mins[pwmindex] + amp) / a;
            }
            if (mins[pwmindex] < 0) {
              mins[pwmindex] = 0;
            }
            int minsize = 30;
            //if (maxs[pwmindex] - mins[pwmindex] < minsize) {
            if (maxs[pwmindex] < minsize) {
              //maxs[pwmindex] = mins[pwmindex] + minsize;
              maxs[pwmindex] = minsize;
            } // if maxs-mins
            if (verbose) fprintf(stdout, "%2d:%3d %f-%f\n", ampbinindex, (int) amp, mins[pwmindex], maxs[pwmindex]);
            if (amp > mins[pwmindex]) {
              double ratio = (amp - mins[pwmindex]) / (maxs[pwmindex] - mins[pwmindex]);
              if (ratio > 1.0) ratio = 1.0;
              ratio *= ratio;
              ratio *= ratio;
              ratio *= ratio;
              ratio *= ratio;
              ratio *= ratio;
              double val = _PCA9685_MAXVAL * ratio;
              //fprintf(stdout, "%f %f\n", ratio, val);
              if (val > 4096) val = 4096;
              int alpha = args.pwm_smoothing;
              double scaled = val < pwmoff[pwmindex] ? ((alpha - 1) * pwmoff[pwmindex] + val) / alpha : val;
              pwmoff[pwmindex] = scaled;
            } else {
              pwmoff[pwmindex] = 0;
            } // if amp
        } // for i
        if (verbose) fprintf(stdout, "\n");
        // update the pwms
        unsigned int pwmon[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        PCA9685_setPWMVals(fd, args.pwm_addr, pwmon, pwmoff);
        { int pwmindex;
          for (pwmindex = 0; pwmindex < 16; pwmindex++) {
            mins[pwmindex] += 0.01;
            maxs[pwmindex] -= 0.1;
          } // for pwmindex
        } // context
      } // if mode 2
    } // else good audio read
    unsigned int a = Microseconds();
    if (a - b > 3000000) {
      unsigned int diff = a - b;
      double ms = (double) diff / 1000.0 / loop;
      printf("%d %0.2f %0.2f  ", loop, ms, 1000.0/ms);
      int j;
      for (j = 0; j < 16; j++) {
        if (bins[j] == 0) continue;
        printf("%3.0f-%-3.0f ", mins[j], maxs[j]);
      } // for j
      printf("\n");
      b = a;
      loop = 0;
    }
    loop++;
    // for testing, to process only one period
    if (testperiod) exit(0);
  } // while 1
} // main
