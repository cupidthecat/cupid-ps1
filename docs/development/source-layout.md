# Source layout

The core keeps each subsystem's headers beside its implementation. CPU code
generation and GPU rendering have separate backend directories.

```text
src/
  core/
    bios/                BIOS identification, patches and executable loading
    cdrom/               Drive commands, sector reading and subchannel data
    config/              Emulation settings
    cpu/                 Interpreter, code cache, recompiler, PGXP and tracing
      backends/
        arm64/           AArch64 dispatcher and interpreter fallback
        x64/             x86-64 emitter and native backend
    database/            Game metadata, track lookup and settings overrides
    dma/                 DMA channels
    gpu/                 Commands, VRAM, display timing and backend interface
      hardware/          OpenGL renderer, shaders and texture cache
      software/          Scalar and SIMD rasterizers
    gte/                 Geometry coprocessor
    input/               Pad ports and multitap
      controllers/       Controller and peripheral implementations
      memory_cards/      Card protocol and image storage
    interrupts/          Interrupt controller
    mdec/                Video decompression and frame dumping
    memory/              Bus mapping, access timing and expansion interface
    scheduler/           Timing events
    serial/              Serial interface
    shared/              Core types
    spu/                 Sound synthesis and CD audio mixing
    state/               Save-state format version
    system/              Boot, reset, frame execution and state coordination
    timers/              Root counters
  common/                Logging, errors, threading, paths and other primitives
  frontend/              SDL2 run loop, command line, hotkeys and input mapping
  util/
    audio/               Output streams, time stretching and WAV files
    config/              INI and YAML readers
    disc/                Disc images, cue sheets and ISO 9660 reading
    executable/          ELF and PS-EXE reading
    graphics/            OpenGL contexts, GPU devices, images and shader helpers
    input/               Host input sources and bindings
    platform/            Host-window metadata, CPU features and page faults
    state/               Serialization, compression and object archives
    text/                Text encoding
```

The current graphics context uses EGL/X11. Native Windows support remains
[planned](building.md#native-windows-support-planned).

Tests follow the same groups under `tests/core/` and `tests/util/`.
`tests/common/` covers shared primitives. The test runner and host stubs live
at the top of `tests/`; Python tests exercise command-line validation and
execution comparisons. `tests/util/graphics/test_gl_context_smoke.c` builds a
separate binary that needs a working display.

Save-state coordination remains in `src/core/system/system.c`. Each subsystem
serializes its own state through the helpers in `src/util/state/`.
`src/core/state/save_state_version.h` defines the format version.

The Makefile discovers nested core, utility and test files. It preserves the
basename order within each source group, excludes `gpu_commands.c` because
`gpu.c` includes it, and keeps the rasterizer's per-file SIMD flags. Header
checks use the same subsystem paths under the selected build directory.
See [building](building.md) and [testing](testing.md).

Other directories:

- `scripts/` contains test helpers, execution comparisons and trace tools.
- `docs/` groups usage and development notes by topic.
- `data/resources/` contains game and disc metadata.
- `dep/` contains bundled dependencies; required notices are in `LICENSE`,
  `licenses/` and the dependency directories.
- `build/` contains generated output. BIOS, games, cards, save states and
  private research stay outside the tracked source tree.
