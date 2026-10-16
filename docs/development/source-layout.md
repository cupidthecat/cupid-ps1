# Source layout

```
src/
├── common/    # logging, error, threading, path, file I/O, types, INI plumbing
├── util/      # CD image readers, audio stream, OpenGL context, GPU device, ISO 9660, INI settings, page-fault handler
├── core/      # CPU, GPU, SPU, CDROM, DMA, IRQ, GTE, timers, controllers, memcards, BIOS, save state, recompiler, game DB
└── frontend/  # SDL2 main loop, CLI parsing, hotkeys, keyboard / gamepad → controller bindings
tests/
├── common/    util/    core/
dep/
├── glad/      libchdr/  lzma/    soundtouch/   stb/
data/
└── resources/ # gamedb.yaml, discsets.yaml, discdb.yaml
```

`core/` contains the emulation subsystems. `frontend/` contains the SDL2 run loop and input mapping.

Notable subsystems:

| Subsystem | Files |
|-----------|-------|
| MIPS interpreter / decode | `src/core/cpu_core.{c,h}`, `src/core/cpu_types.{c,h}` |
| Recompiler core | `src/core/cpu_recompiler.{c,h}`, `src/core/cpu_code_cache.{c,h}` |
| x86-64 backend | `src/core/x64_emit.{c,h}`, `src/core/x64_backend.{c,h}` |
| AArch64 backend (interpreter fallback) | `src/core/arm64_emit.{c,h}`, `src/core/arm64_backend.{c,h}` |
| GTE coprocessor | `src/core/gte.{c,h}`, `src/core/gte_types.h` |
| GPU command processor | `src/core/gpu.{c,h}`, `src/core/gpu_commands.c` |
| Software rasterizer | `src/core/gpu_sw.{c,h}`, `src/core/gpu_sw_rasterizer.{c,h}`, `gpu_sw_rasterizer_simd.c`, `gpu_sw_rasterizer_avx2.c` |
| Hardware OpenGL | `src/core/gpu_hw.{c,h}`, `src/core/gpu_hw_shadergen.{c,h}`, `src/core/gpu_hw_texture_cache.{c,h}`, `src/core/tc_image_loader.{c,h}` |
| CPU feature detect | `src/util/cpu_features.{c,h}` |
| SPU | `src/core/spu.{c,h}` |
| CDROM drive | `src/core/cdrom.{c,h}`, `src/core/cdrom_async_reader.{c,h}`, `src/core/cdrom_subq_replacement.{c,h}` |
| Disc image readers | `src/util/cd_image*.c` (cue, chd, mds, ccd, pbp, ppf, m3u, ecm, device, memory) |
| Bus / address decode | `src/core/bus.{c,h}` |
| System lifecycle / save state | `src/core/system.{c,h}`, `src/core/save_state_version.h` |
| Frontend | `src/frontend/main.c`, `src/frontend/input_map.{c,h}` |
| Settings | `src/core/settings.{c,h}`, `src/util/ini_settings_interface.{c,h}` |
| BIOS | `src/core/bios.{c,h}` |
| Game database | `src/core/game_database.{c,h}`, `src/core/game_database_apply.c`, `src/core/game_database_tracks.c` |
| FMV dump | `src/core/fmv_dump.{c,h}` |
| Diff tooling | `scripts/compare_execution.py`, `src/core/cpu_diff_runner.{c,h}`, `src/core/cpu_execution_stats.{c,h}`, `src/core/cpu_pc_trigger.{c,h}` |
| ELF/PSEXE reader | `src/util/elf_file.{c,h}` |
| Page-fault handler | `src/util/page_fault_handler.c` |
| OpenGL context (EGL/X11) | `src/util/opengl_context_egl.{c,h}`, `src/util/opengl_context_egl_xlib.c` |
