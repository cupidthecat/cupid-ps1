# Audio output

| Setting | Source | Default |
|------|--------|---------|
| Backend | `src/util/audio/sdl_audio_stream.c` | SDL2 |
| Sample rate | (PSX native) | 44 100 Hz stereo |
| Stretch mode | `src/util/audio/audio_stream.h` | `TimeStretch` (SoundTouch) |
| Buffer | `src/core/config/settings.h` | 50 ms |
| Output latency | `src/core/config/settings.h` | 20 ms |

Stretch modes:

- `Off`: emit guest samples raw.
- `Resample`: basic resample to host rate.
- `TimeStretch`: pitch-preserving time-stretch via SoundTouch. Default.

The SPU implementation lives in `src/core/spu/spu.c` (voices, ADSR, reverb, CD-DA mixing). The output ring is a soft-capped streaming buffer in `src/util/audio/audio_stream.c`.
