/* cupid-ps1 native C11 audio dependency interface check. */
#include <soundtouch/SoundTouchDLL.h>

#include <math.h>
#include <stdio.h>

int main(void)
{
  if (soundtouch_getVersionId() != 20303) return 1;
  ST_HANDLE handle = soundtouch_createInstance();
  if (!handle) return 2;
  soundtouch_setChannels(handle, 2);
  soundtouch_setSampleRate(handle, 44100);
  soundtouch_setTempo(handle, 1.0f);
  soundtouch_setRate(handle, 1.0f);
  float input[4096 * 2];
  for (unsigned i = 0; i < 4096; i++) {
    input[i * 2]     = ((int)(i % 127) - 63) / 64.0f;
    input[i * 2 + 1] = -input[i * 2];
  }
  soundtouch_putSamples(handle, input, 4096);
  soundtouch_flush(handle);
  unsigned frames = 0;
  double   energy = 0;
  float    output[512 * 2];
  unsigned count;
  while ((count = soundtouch_receiveSamples(handle, output, 512)) != 0) {
    if (count > 512 || frames + count > 4096) return 3;
    frames += count;
    for (unsigned i = 0; i < count * 2; i++) {
      if (!isfinite(output[i])) return 4;
      energy += (double)output[i] * output[i];
    }
  }
  soundtouch_clear(handle);
  const int empty = soundtouch_isEmpty(handle);
  soundtouch_destroyInstance(handle);
  if (frames != 4096 || energy == 0 || !empty) return 5;
  printf("SoundTouch C11 interface: version=20303 stereo_frames=%u finite_nonzero_output=yes\n", frames);
  return 0;
}
