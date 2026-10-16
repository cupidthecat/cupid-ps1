# Frontend usage

## Running

```
cupid-ps1 [options] <game-path>
```

`<game-path>` is a single positional argument: a `.cue`, `.bin`, `.iso`, `.img`, `.ecm`, `.chd`, `.m3u`, `.mds`, `.ccd`, `.pbp` file, or a Linux block-device path (e.g. `/dev/sr0`). Multiple games are not accepted.

### Command-line options

| Flag | Description | Default |
|------|-------------|---------|
| `--bios PATH` | BIOS image path. | Falls back to `./SCPH1001.BIN`, then `./SCPH7003.bin`. Exits if neither is found. |
| `--gpu-renderer NAME` | `software`/`sw`, `opengl`/`gl`/`hardware`/`hw`, or `automatic`/`auto`. | settings.ini default (`Software`). |
| `--gpu-resolution-scale N` | Internal resolution multiplier for the HW backend. `1`..`16`. SW renderer ignores it. | `1` |
| `--cdrom-precache` | Decompress the entire disc image into RAM at boot. Eliminates per-hunk decode for CHD. Auto-applied for CHD-backed `.m3u` entries. | off |
| `--cpu-mode MODE` | `interp`/`interpreter`, `cached`/`cached-interpreter`, `recomp`/`recompiler`. | settings.ini default (`recompiler`). |
| `--fastmem MODE` | `off`/`disabled`, `lut`, `mmap`/`hw`. Recompiler load/store dispatch strategy. | settings.ini default (typically `lut`). |
| `--pgxp on\|off` | Enable PGXP subpixel-precision geometry. | settings.ini default (off). |
| `--no-block-linking` | Disable recompiler block linking. Bisect knob. | linking on |
| `--no-icache` | Disable recompiler icache emulation. Bisect knob. | icache on |
| `--test-savestate-roundtrip` | Saves after frame 300, reloads and requests 60 more frames. Requires local runtime qualification. | off |
| `-h`, `--help` | Show usage. | n/a |

CLI flags override settings INI for the current run.

## Hotkeys

| Key | Action |
|-----|--------|
| `Esc` | Quit. |
| `P` | Pause / resume. |
| `F5` | Reset (warm reboot of the running disc). |
| `Alt+Enter` | Toggle fullscreen (desktop fullscreen, no resolution change). |
| `Tab` (held) | Fast-forward; uncaps the speed limiter while held; releases on key-up. |
| `F1` | Toggle ANALOG button on controller 1 (DualShock analog/digital mode). |
| `F3` | Previous sub-image (multidisc `.m3u`). |
| `F4` | Next sub-image (multidisc `.m3u`). |
| `Shift+1` ... `Shift+9` | Pick the active save-state slot (default `1`). Logged to stderr. |
| `F11` | Save state to the active slot (global save, i.e. `savestates/savestate_<slot>.sav`). |
| `F12` | Load state from the active slot. |

Caveats:
- `Tab` is also bound to `MODE` on the Jogcon controller (`src/frontend/input_map.c`); fast-forward and Jogcon MODE will both fire when Jogcon is selected.
- `Alt+Enter` lets the underlying `Return` keypress through to the pad for one frame, briefly registering `Start`. Negligible in practice.

## Default keyboard mapping

Defaults are set in `src/frontend/input_map.c` for each controller type when no INI bindings exist. Players typically use the analog (DualShock) layout.

### Analog / Digital controller

| PS1 button | Key |
|------------|-----|
| D-Pad ↑↓←→ | Arrow keys |
| Cross / Circle / Square / Triangle | `Z` / `X` / `A` / `S` |
| L1 / L2 / R1 / R2 | `LShift` / `LCtrl` / `RShift` / `RCtrl` |
| Start / Select | `Return` / `Backspace` |
| Left stick | `A`/`D` (X), `W`/`S` (Y) |
| Right stick | `J`/`L` (X), `I`/`K` (Y) |

### neGcon

D-pad / Start mapped as above. Steering on `A`/`D`, throttle on `W`, brake on `S`, L paddle on `LShift`, A/B/R buttons on `Z`/`X`/`RShift`.

### Jogcon

Standard pad layout plus `Tab` for MODE and `Q`/`E` for jog-wheel left/right.

### Densha de Go

`Return`/`Backspace` for Start/Select. `Z`/`X`/`C` for A/B/C. Number row drives the master controller: `` ` `` (power-off), `1`..`5` (power notches), `6`..`9` and `0`/`-`/`=` (brake notches).

### SDL gamepads

Hot-plug supported via udev. Buttons map naturally (Xbox-style layout): A/B/X/Y → Cross/Circle/Square/Triangle, dpad, sticks, triggers (analog → L2/R2 with 8192 deadzone threshold), L3/R3, GUIDE → ANALOG.

### Mouse / lightgun

When controller 1 is `GunCon` or `Justifier`, SDL mouse motion drives the pointer (Trigger = left click, A/Start = right click, B/Back = middle click, ShootOffscreen = `RShift`). When controller 1 is `PlayStationMouse`, SDL mouse delta is accumulated and drained as the relative pointer over SIO; left/right mouse buttons map to the two PS Mouse buttons.

## Environment variables

All `CUPID_*` env vars are read at startup unless noted otherwise.

| Variable | Effect |
|----------|--------|
| `CUPID_DIGITAL_PAD` | Set → controller 1 is a digital pad instead of DualShock. |
| `CUPID_NO_MEMCARD` | Set → no memory card in slot 1. |
| `CUPID_FASTBOOT=1` | Enable BIOS fast-boot patch. Off by default (some titles black-screen with it on). |
| `CUPID_OLD_MDEC` | Set → select the legacy floating-point MDEC decoder instead of the integer decoder. Diagnostic. |
| `CUPID_NO_CHROMA_SMOOTH` | Set → disable 24-bit FMV chroma smoothing. Diagnostic. |
| `CUPID_TRACE` | Set → bumps log level to `DEV` and emits sampled (every 64th frame) CPU/GPU/IRQ/DMA/timer/CD/SPU state dumps. Polled at runtime. |
| `CUPID_TRACE_PC` | Implies `CUPID_TRACE`. Adds a CPU PC-window dump per sample. |
| `CUPID_TRACE_TASK` | Implies `CUPID_TRACE`. Dumps BIOS task state when PC is in the dispatcher range. |
| `CUPID_TRACE_CD` | Enable CDROM command tracing (independent of `CUPID_TRACE`). |
| `CUPID_TRACE_DISP` | Trace presentation / display-region changes. |
| `CUPID_TRACE_GP0=PATH` | Dump GPU GP0 command stream to `PATH`. |
| `CUPID_TRACE_EXEC` | Per-block recompiler execution trace. |
| `CUPID_TRACE_STEP_DIFF` | Single-step interpreter cross-check on every instruction. |
| `CUPID_TRACE_LOW_RAM` / `_RANGE` / `_LOG` / `_RING` / `_TRIP_PADDR` / `_BCDTRIP` / `_SKIP_WRITERS` | Low-RAM write-watch debugging facility. |
| `CUPID_PC_TRIGGER_PC` / `_LIMIT` / `_LOG` / `_OPCODE` | PC-trigger module: dump CPU state when PC (or opcode) is hit. |
| `CUPID_RECOMP_DIFF_KEEP_LINKING` / `_KEEP_FASTMEM` / `_TICKS` | Recompiler-diff harness knobs. |
| `CUPID_TEXTURE_CACHE` | Enable HW GPU texture-cache trace. |
| `CUPID_GPU_SW_ISA` | Force SW rasterizer backend: `SCALAR`, `SIMD` (SSE4.1), or `AVX2`. Falls back to highest available if requested ISA is missing. Default: auto-detect. |
| `CUPID_FMV_DUMP=PATH` | Dump MDEC frames to `PATH/`. Requires the binary built with `CUPID_FMV_DUMP=1`. |
| `CUPID_DIFF_TRACE=PATH` | Per-instruction interpreter trace output path. Used by `make test-diff`. |
| `CUPID_RECOMP_DIFF=1` | Recompiler register-file diff mode. Used by `make recomp-diff-test`. |
| `SDL_VIDEODRIVER=dummy`, `SDL_AUDIODRIVER=dummy` | Standard SDL knobs for headless runs (used by the test harness). |

## Settings

INI-format settings file via `src/util/ini_settings_interface.c`. The settings struct is `g_settings` in `src/core/settings.{h,c}`; defaults are listed by `SETTINGS_DEFAULT_*` macros.

### `./settings.ini`

The frontend reads `./settings.ini` (in the current working directory) at startup.

- **First run**: if no `settings.ini` exists, the frontend writes one with the current defaults and logs `[settings] wrote default ./settings.ini`. Edit it freely between runs.
- **Subsequent runs**: the file is parsed and overlaid onto the defaults (`[settings] loaded ./settings.ini`). Missing keys keep their built-in default; unparseable files fall back to defaults with a warning.
- **CLI overrides win**: command-line flags (`--cpu-mode`, `--fastmem`, `--pgxp`, `--gpu-renderer`, `--gpu-resolution-scale`, `--cdrom-precache`) are applied *after* the INI load, so they always take effect for the current run without modifying the file.
- **Env-var overrides win**: `CUPID_DIGITAL_PAD`, `CUPID_NO_MEMCARD`, `CUPID_FASTBOOT`, `CUPID_OLD_MDEC`, `CUPID_NO_CHROMA_SMOOTH` likewise overlay after the INI load.

The on-disk format is plain key/value INI grouped by section (`[Audio]`, `[BIOS]`, `[CPU]`, `[GPU]`, `[Pad1]`, `[MemoryCards]`, ...). Section/key names are stable across runs; the file is written sorted, so diffs are minimal.

Selected defaults:

| Setting | Default | Macro |
|---------|---------|-------|
| GPU renderer | `Software` | `SETTINGS_DEFAULT_GPU_RENDERER` |
| GPU dithering | `TrueColor` (dithering off) | `SETTINGS_DEFAULT_GPU_DITHERING_MODE` |
| CPU execution mode | `Recompiler` | `SETTINGS_DEFAULT_CPU_EXECUTION_MODE` |
| Controller 1 type | `AnalogController` | `SETTINGS_DEFAULT_CONTROLLER_1_TYPE` |
| Memory card 1 type | `PerGameTitle` | `SETTINGS_DEFAULT_MEMORY_CARD_1_TYPE` |
| Audio stretch | `TimeStretch` | `SETTINGS_AUDIO_DEFAULT_STRETCH_MODE` |
| Audio buffer | 50 ms | `SETTINGS_AUDIO_DEFAULT_BUFFER_MS` |
| Audio output latency | 20 ms | `SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MS` |

CLI flags (`--cpu-mode`, `--fastmem`, `--pgxp`, `--gpu-renderer`, `--gpu-resolution-scale`, `--cdrom-precache`) override settings INI for the current run.
