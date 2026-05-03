/*
 * cupid-ps1 SDL2 frontend.
 *
 * Responsibilities:
 *   - argv parsing (--bios, game cue path)
 *   - SDL init (video / audio / gamecontroller)
 *   - settings_t population and BIOS handoff
 *   - audio_stream creation and binding to the SPU
 *   - boot the system, run system_execute(), present each frame
 *   - implement the host_* hooks that the core calls back into
 */

#include "frontend/input_map.h"

#include "core/analog_controller.h"
#include "core/bios.h"
#include "core/controller.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_disasm.h"
#include "core/cdrom.h"
#include "core/dma.h"
#include "core/gpu.h"
#include "core/gpu_sw.h"
#include "core/gpu_hw.h"
#include "core/gte.h"
#include "util/cpu_features.h"
#include "util/opengl_context.h"
#include "util/opengl_device.h"
#include "util/window_info.h"
#include "glad/gl.h"
#include <SDL_syswm.h>
#include "core/host.h"
#include "core/interrupt_controller.h"
#include "core/mdec.h"
#include "core/pad.h"
#include "core/settings.h"
#include "core/spu.h"
#include "core/system.h"
#include "core/timers.h"

#include "common/crash_handler.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/small_string.h"
#include "common/types.h"

#include "util/audio_stream.h"
#include "util/core_audio_stream.h"
#include "util/ini_settings_interface.h"

#include <SDL.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

LOG_CHANNEL(Host);

int  main(int argc, char** argv);

/* host hook overrides (non-weak; link before system.c's weak defaults). */
void host_pump_messages_on_core_thread(void);
void host_input_manager_poll_sources  (void);
void host_wait_for_all_async_tasks    (void);

/* hooks consumed by core/bios.c (declared weak there). */
const char* emu_folders_get_bios(void);
void        core_get_string_setting_value(const char* section, const char* key,
                                          const char* def, small_string_t* out);

 /* Internal forward decls so -Wmissing-prototypes is happy on the
 * file-static helpers. */
static void trace_disasm_window(u32 center, char marker);
static void trace_pc_window(void);
static void trace_task_entry(const char* label, u32 addr);
static void trace_task_state(void);

typedef struct {
  /* SDL handles. */
  SDL_Window*   window;
  SDL_Renderer* renderer;
  SDL_Texture*  texture;
  u32           texture_width;
  u32           texture_height;

  /* Audio. */
  core_audio_stream_t* audio;

  /* CLI inputs. */
  char* bios_path;
  char* game_path;
  /* Renderer requested via --gpu-renderer (overrides INI/default).  When set
   * to GPU_RENDERER_HARDWARE_OPENGL but the HW factory isn't compiled in
   * yet, we log a warning and fall back to software. */
  gpu_renderer_t requested_renderer;
  bool           renderer_requested;   /* explicit override vs. default */

  /* --gpu-resolution-scale CLI knob.  When set, overrides the INI
   * GPU.ResolutionScale.  Range 1..16 (the GL backend clamps further on
   * texture-size limits at create time). */
  u8             requested_resolution_scale;
  bool           resolution_scale_requested;

  /* Cached settings owner copies (BIOS dir / mcd path). */
  char* bios_dir;

  /* --cdrom-precache: load whole disc image into RAM at boot. Eliminates
   * per-hunk decompress cost for CHD images at the cost of ~600MB RAM. */
  bool cdrom_precache;

  /* --cpu-mode: select INTERPRETER / CACHED_INTERPRETER / RECOMPILER at
   * startup, without editing settings.ini. */
  bool                 cpu_mode_requested;
  cpu_execution_mode_t requested_cpu_mode;

  /* --fastmem off|lut|mmap.  Picks the recompiler's load/store dispatch
   * strategy.  off = always slow-path C thunk; lut = per-page LUT lookup in
   * emitted code; mmap = SIGSEGV-backed direct host addressing. */
  bool                 fastmem_requested;
  cpu_fastmem_mode_t   requested_fastmem_mode;

  /* --pgxp on|off.  Enables PGXP shadow tracking + GPU vertex subpixel
   * correction. */
  bool                 pgxp_requested;
  bool                 requested_pgxp_enable;

  /* Bisect knobs: force off recompiler subsystems individually so we can
   * tell which one produces a bad PC.  Each `*_off` flag overrides
   * g_settings to false/disabled regardless of settings.ini. */
  bool                 no_block_linking;
  bool                 no_icache;

  /* --test-savestate-roundtrip: at frame ~300 (5s @ 60fps) save state to a
   * tmp path, immediately load it back, run 60 more frames, exit 0.  Smokes
   * the cpu/bus/pgxp DoState bodies end-to-end. */
  bool test_rt_enabled;
  bool test_rt_saved;
  u32  test_rt_quit_frame;

  /* Run-loop state. */
  bool quit_requested;

  /* QoL hotkey state. */
  bool fullscreen;          /* Alt+Enter toggle */
  bool fast_forward_held;   /* Tab held - drives system_set_fast_forward_enabled */
  s32  current_save_slot;   /* Shift+1..9 picks; F11 saves, F12 loads */

  /* INI-backed user settings.  Initialised on first call to
   * load_or_create_settings_ini(); destroyed in main() at exit. */
  ini_settings_interface_t ini;
  bool                     ini_inited;
} frontend_state_t;

static frontend_state_t s_fe;
static volatile sig_atomic_t s_dump_on_exit = 0;

static void dump_debug_ringbuffers(void)
{
  fputs("\n========= debug ringbuffer dump =========\n", stderr);
  cdrom_dbg_dump_recent_commands(stderr);
  fputc('\n', stderr);
  gpu_dbg_dump_recent_gp1(stderr);
  fputc('\n', stderr);
  gpu_dbg_dump_recent_gp0(stderr);
  fputc('\n', stderr);
  cpu_code_cache_dbg_dump_recent_blocks(stderr);
  fputc('\n', stderr);
  cpu_code_cache_dbg_low_ram_dump();
  fputc('\n', stderr);
  spu_dbg_dump_counters(stderr);
  fputs("=========================================\n", stderr);
  fflush(stderr);
}

static void sigint_handler(int sig)
{
  (void)sig;
  /* Async-signal-safe path: write a marker, then exit() so atexit handlers run. */
  static const char marker[] = "\n[sigint] dumping debug ringbuffers via atexit...\n";
  (void)write(2, marker, sizeof(marker) - 1);
  s_dump_on_exit = 1;
  exit(0);
}

static void print_usage(FILE* fp, const char* argv0)
{
  fprintf(fp,
    "Usage: %s [--bios /path/to/SCPH1001.BIN] <game.cue>\n"
    "\n"
    "Options:\n"
    "  --bios PATH         Override BIOS image path.  If omitted, falls back to\n"
    "                      ./SCPH1001.BIN then ./SCPH7003.bin in the current dir.\n"
    "  --gpu-renderer NAME Backend selection: software (default), opengl, automatic.\n"
    "                      OpenGL falls back to software until the hardware\n"
    "                      factory lands.\n"
    "  --gpu-resolution-scale N\n"
    "                      Internal resolution multiplier for the OpenGL backend.\n"
    "                      1..16 (default 1).  Ignored for the software backend.\n"
    "  --cdrom-precache    Decompress the whole disc image into RAM at boot.\n"
    "                      Eliminates per-hunk decode cost for .chd images.\n"
    "  --cpu-mode MODE     interp | cached | recomp (default: interp).\n"
    "  --fastmem MODE      off | lut | mmap (recompiler load/store dispatch;\n"
    "                      mmap uses SIGSEGV-backed direct host addressing).\n"
    "  --pgxp on|off       Enable PGXP subpixel-precision tracking.\n"
    "  --no-block-linking  Disable recompiler block linking (bisect knob).\n"
    "  --no-icache         Disable recompiler icache emulation (bisect knob).\n"
    "  -h, --help          Show this message.\n",
    argv0 ? argv0 : "cupid-ps1");
}

static bool file_exists(const char* path)
{
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char* xstrdup(const char* s)
{
  if (!s) return NULL;
  const size_t n = strlen(s);
  char* r = (char*)malloc(n + 1);
  if (!r) {
    fprintf(stderr, "out of memory\n");
    abort();
  }
  memcpy(r, s, n + 1);
  return r;
}

static bool parse_args(int argc, char** argv)
{
  for (int i = 1; i < argc; i++) {
    const char* a = argv[i];
    if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
      print_usage(stdout, argv[0]);
      return false;
    }
    if (strcmp(a, "--bios") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "%s: --bios requires an argument.\n", argv[0]);
        return false;
      }
      free(s_fe.bios_path);
      s_fe.bios_path = xstrdup(argv[++i]);
      continue;
    }
    if (strcmp(a, "--gpu-renderer") == 0 || strncmp(a, "--gpu-renderer=", 15) == 0) {
      const char* val = (a[14] == '=') ? (a + 15) : (i + 1 < argc ? argv[++i] : NULL);
      if (!val) {
        fprintf(stderr, "%s: --gpu-renderer requires an argument.\n", argv[0]);
        return false;
      }
      gpu_renderer_t r;
      if      (strcasecmp(val, "automatic") == 0 || strcasecmp(val, "auto") == 0)
        r = GPU_RENDERER_AUTOMATIC;
      else if (strcasecmp(val, "software")  == 0 || strcasecmp(val, "sw")   == 0)
        r = GPU_RENDERER_SOFTWARE;
      else if (strcasecmp(val, "opengl")    == 0 || strcasecmp(val, "gl")   == 0 ||
               strcasecmp(val, "hardware")  == 0 || strcasecmp(val, "hw")   == 0)
        r = GPU_RENDERER_HARDWARE_OPENGL;
      else if (!settings_parse_renderer_name(val, &r)) {
        fprintf(stderr, "%s: unknown renderer '%s' (valid: automatic, software, opengl).\n",
                argv[0], val);
        return false;
      }
      s_fe.requested_renderer = r;
      s_fe.renderer_requested = true;
      continue;
    }
    if (strcmp(a, "--gpu-resolution-scale") == 0 ||
        strncmp(a, "--gpu-resolution-scale=", 23) == 0) {
      const char* val = (a[22] == '=') ? (a + 23) : (i + 1 < argc ? argv[++i] : NULL);
      if (!val) {
        fprintf(stderr, "%s: --gpu-resolution-scale requires an argument.\n", argv[0]);
        return false;
      }
      char* endp = NULL;
      const long n = strtol(val, &endp, 10);
      if (!endp || *endp != '\0' || n < 1 || n > 16) {
        fprintf(stderr, "%s: --gpu-resolution-scale must be an integer 1..16 (got '%s').\n",
                argv[0], val);
        return false;
      }
      s_fe.requested_resolution_scale = (u8)n;
      s_fe.resolution_scale_requested = true;
      continue;
    }
    if (strcmp(a, "--cdrom-precache") == 0) {
      s_fe.cdrom_precache = true;
      continue;
    }
    if (strcmp(a, "--cpu-mode") == 0 || strncmp(a, "--cpu-mode=", 11) == 0) {
      const char* val = (a[10] == '=') ? (a + 11) : (i + 1 < argc ? argv[++i] : NULL);
      if (!val) {
        fprintf(stderr, "%s: --cpu-mode requires an argument.\n", argv[0]);
        return false;
      }
      cpu_execution_mode_t m;
      if      (strcasecmp(val, "interp")   == 0 || strcasecmp(val, "interpreter") == 0)
        m = CPU_EXECUTION_MODE_INTERPRETER;
      else if (strcasecmp(val, "cached")   == 0 || strcasecmp(val, "cached-interpreter") == 0)
        m = CPU_EXECUTION_MODE_CACHED_INTERPRETER;
      else if (strcasecmp(val, "recomp")   == 0 || strcasecmp(val, "recompiler") == 0)
        m = CPU_EXECUTION_MODE_RECOMPILER;
      else {
        fprintf(stderr, "%s: unknown cpu mode '%s' (valid: interp, cached, recomp).\n",
                argv[0], val);
        return false;
      }
      s_fe.requested_cpu_mode = m;
      s_fe.cpu_mode_requested = true;
      continue;
    }
    if (strcmp(a, "--fastmem") == 0 || strncmp(a, "--fastmem=", 10) == 0) {
      const char* val = (a[9] == '=') ? (a + 10) : (i + 1 < argc ? argv[++i] : NULL);
      if (!val) {
        fprintf(stderr, "%s: --fastmem requires an argument.\n", argv[0]);
        return false;
      }
      cpu_fastmem_mode_t fm;
      if      (strcasecmp(val, "off")      == 0 || strcasecmp(val, "disabled") == 0)
        fm = CPU_FASTMEM_MODE_DISABLED;
      else if (strcasecmp(val, "lut")      == 0)
        fm = CPU_FASTMEM_MODE_LUT;
      else if (strcasecmp(val, "mmap")     == 0 || strcasecmp(val, "hw") == 0)
        fm = CPU_FASTMEM_MODE_MMAP;
      else {
        fprintf(stderr, "%s: unknown fastmem mode '%s' (valid: off, lut, mmap).\n",
                argv[0], val);
        return false;
      }
      s_fe.requested_fastmem_mode = fm;
      s_fe.fastmem_requested      = true;
      continue;
    }
    if (strcmp(a, "--pgxp") == 0 || strncmp(a, "--pgxp=", 7) == 0) {
      const char* val = (a[6] == '=') ? (a + 7) : (i + 1 < argc ? argv[++i] : NULL);
      if (!val) {
        fprintf(stderr, "%s: --pgxp requires an argument.\n", argv[0]);
        return false;
      }
      bool on;
      if      (strcasecmp(val, "on")  == 0 || strcasecmp(val, "true")  == 0 ||
               strcasecmp(val, "1")   == 0 || strcasecmp(val, "yes")   == 0)
        on = true;
      else if (strcasecmp(val, "off") == 0 || strcasecmp(val, "false") == 0 ||
               strcasecmp(val, "0")   == 0 || strcasecmp(val, "no")    == 0)
        on = false;
      else {
        fprintf(stderr, "%s: --pgxp expects on|off (got '%s').\n", argv[0], val);
        return false;
      }
      s_fe.requested_pgxp_enable = on;
      s_fe.pgxp_requested        = true;
      continue;
    }
    if (strcmp(a, "--test-savestate-roundtrip") == 0) {
      s_fe.test_rt_enabled = true;
      continue;
    }
    if (strcmp(a, "--no-block-linking") == 0) {
      s_fe.no_block_linking = true;
      continue;
    }
    if (strcmp(a, "--no-icache") == 0) {
      s_fe.no_icache = true;
      continue;
    }
    if (a[0] == '-') {
      fprintf(stderr, "%s: unknown option '%s'.\n", argv[0], a);
      print_usage(stderr, argv[0]);
      return false;
    }
    if (s_fe.game_path) {
      fprintf(stderr, "%s: more than one game path specified.\n", argv[0]);
      return false;
    }
    s_fe.game_path = xstrdup(a);
  }

  if (!s_fe.game_path) {
    fprintf(stderr, "%s: no game image specified.\n", argv[0]);
    print_usage(stderr, argv[0]);
    return false;
  }

  if (!s_fe.bios_path) {
    if (file_exists("./SCPH1001.BIN"))
      s_fe.bios_path = xstrdup("./SCPH1001.BIN");
    else if (file_exists("./SCPH7003.bin"))
      s_fe.bios_path = xstrdup("./SCPH7003.bin");
    else {
      fprintf(stderr,
              "%s: no BIOS specified and neither ./SCPH1001.BIN nor "
              "./SCPH7003.bin exists in the current directory.\n", argv[0]);
      return false;
    }
  } else if (!file_exists(s_fe.bios_path)) {
    fprintf(stderr, "%s: BIOS file '%s' not found.\n", argv[0], s_fe.bios_path);
    return false;
  }

  return true;
}

 /* Splits "./dir/file.ext" -> dirname "./dir", filename "file.ext".  Both
 * out_dir and out_file are heap-owned; caller frees. */
static void split_path(const char* full, char** out_dir, char** out_file)
{
  const char* slash = strrchr(full, '/');
  if (!slash) {
    *out_dir  = xstrdup(".");
    *out_file = xstrdup(full);
    return;
  }
  const size_t dir_len = (size_t)(slash - full);
  if (dir_len == 0) {
    *out_dir = xstrdup("/");
  } else {
    char* d = (char*)malloc(dir_len + 1);
    if (!d) abort();
    memcpy(d, full, dir_len);
    d[dir_len] = '\0';
    *out_dir = d;
  }
  *out_file = xstrdup(slash + 1);
}

static bool ensure_dir(const char* path)
{
  struct stat st;
  if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
  Error e; Error_init(&e);
  const bool ok = fs_create_directory(path, true, &e);
  if (!ok) {
    fprintf(stderr, "mkdir '%s' failed: %s\n", path, Error_get_description(&e));
  }
  Error_destroy(&e);
  return ok;
}

const char* emu_folders_get_bios(void)
{
  return s_fe.bios_dir ? s_fe.bios_dir : ".";
}

void core_get_string_setting_value(const char* section, const char* key,
                                   const char* def, small_string_t* out)
{
  /* The only key bios.c actually reads is one of [BIOS]
   * PathNTSCU / PathNTSCJ / PathPAL.  Return the user-supplied filename
   * for any of them so the BIOS loader picks up our --bios path. */
  (void)section; (void)key; (void)def;
  const char* path = s_fe.bios_path ? s_fe.bios_path : "";
  const char* slash = strrchr(path, '/');
  const char* fname = slash ? (slash + 1) : path;
  small_string_assign_cstr(out, fname);
}

/* INI bootstrap: load ./settings.ini into g_settings if it exists, otherwise
 * write current g_settings out as a fresh defaults file.  Idempotent - only
 * the first call does work; later calls are no-ops.  Must run AFTER
 * settings_init seeds defaults (so "first run" writes those defaults to
 * disk) and BEFORE the CLI/env overrides are applied (so flags win). */
static void load_or_create_settings_ini(void)
{
  if (s_fe.ini_inited)
    return;
  ini_settings_interface_init(&s_fe.ini, "./settings.ini");
  s_fe.ini_inited = true;

  if (file_exists("./settings.ini")) {
    Error e = ERROR_INIT;
    if (!ini_settings_interface_load(&s_fe.ini, &e)) {
      fprintf(stderr, "[settings] could not parse ./settings.ini (%s); using defaults.\n",
              Error_get_description(&e));
    } else {
      settings_load(&g_settings, &s_fe.ini.base, &s_fe.ini.base);
      fprintf(stderr, "[settings] loaded ./settings.ini\n");
    }
    Error_destroy(&e);
    return;
  }

  /* First-run bootstrap: serialize current defaults to disk so the user has
   * a starting INI to edit. */
  settings_save(&g_settings, &s_fe.ini.base, /*ignore_base*/ false);
  Error e = ERROR_INIT;
  if (!ini_settings_interface_save(&s_fe.ini, &e)) {
    fprintf(stderr, "[settings] could not write ./settings.ini: %s\n",
            Error_get_description(&e));
  } else {
    fprintf(stderr, "[settings] wrote default ./settings.ini\n");
  }
  Error_destroy(&e);
}

static void apply_frontend_settings(void)
{
  settings_init(&g_settings);

  /* MDEC outputs YUV 4:2:0; chroma is 8x8 block resolution, so 24-bit FMV
   * (Capcom logo, intros, anything that goes through CD->MDEC) shows visible
   * colour-block edges without smoothing.  Default-on; INI may flip off. */
  g_settings.display_24bit_chroma_smoothing = true;

  /* Overlay user settings from ./settings.ini (or write defaults if missing).
   * Runs after settings_init so missing INI keys keep our seeded defaults;
   * runs before CLI/env overrides so flags still win. */
  load_or_create_settings_ini();

  /* --cpu-mode CLI override.  Default keeps INI/settings_init value
   * (CPU_EXECUTION_MODE_RECOMPILER).  If recomp init fails (rare: only when
   * the host can't allocate the 48 MB JIT region), system_initialize demotes
   * to interpreter and the run continues. */
  if (s_fe.cpu_mode_requested)
    g_settings.cpu_execution_mode = s_fe.requested_cpu_mode;

  /* --fastmem CLI override.  Default tracks settings.ini /
   * SETTINGS_DEFAULT_CPU_FASTMEM_MODE. */
  if (s_fe.fastmem_requested)
    g_settings.cpu_fastmem_mode = s_fe.requested_fastmem_mode;

  /* --pgxp CLI override.  Default leaves settings.ini value intact. */
  if (s_fe.pgxp_requested)
    g_settings.gpu_pgxp_enable = s_fe.requested_pgxp_enable;

  /* Bisect knobs.  No "on" form; the production setting is already on
   * by default; these only let the operator force-off for repro. */
  if (s_fe.no_block_linking)
    g_settings.cpu_recompiler_block_linking = false;
  if (s_fe.no_icache)
    g_settings.cpu_recompiler_icache = false;

  /* GPU renderer selection.  Without a CLI override we keep the INI value
   * (defaulting to SOFTWARE); collapse AUTOMATIC to SOFTWARE so sdl_setup()
   * skips GL bring-up.  HW renderer stays in tree but is opt-in only via
   * --gpu-renderer opengl or `Renderer = OpenGL` in settings.ini; the FS
   * pipeline path has 28 documented hybrid/band-aid sites pending the GL
   * takeover.  SW backend (with AVX2 dispatch) is the supported default. */
  if (s_fe.renderer_requested) {
    g_settings.gpu_renderer = (s_fe.requested_renderer == GPU_RENDERER_HARDWARE_OPENGL)
                                ? GPU_RENDERER_HARDWARE_OPENGL
                                : GPU_RENDERER_SOFTWARE;
  } else if (g_settings.gpu_renderer == GPU_RENDERER_AUTOMATIC) {
    g_settings.gpu_renderer = GPU_RENDERER_SOFTWARE;
  }
  g_settings.gpu_use_thread      = false;

  /* --gpu-resolution-scale CLI override.  Only meaningful for the
   * GL backend; the SW backend ignores g_settings.gpu_resolution_scale. */
  if (s_fe.resolution_scale_requested) {
    g_settings.gpu_resolution_scale = s_fe.requested_resolution_scale;
    g_settings.gpu_automatic_resolution_scale = false;
  }

  /* Controller 1 = analog (DualShock). Crash-era titles should also accept a
   * digital pad, so expose an env toggle while we shake out pad handshakes. */
  g_settings.controller_types[0] = getenv("CUPID_DIGITAL_PAD") ?
                                     CONTROLLER_TYPE_DIGITAL_CONTROLLER : 
                                     CONTROLLER_TYPE_ANALOG_CONTROLLER;

  g_settings.memory_card_types[0] = getenv("CUPID_NO_MEMCARD") ?
                                      MEMORY_CARD_TYPE_NONE : 
                                      MEMORY_CARD_TYPE_SHARED;

  free(g_settings.memory_card_paths[0]);
  g_settings.memory_card_paths[0] = (g_settings.memory_card_types[0] == MEMORY_CARD_TYPE_NONE) ?
                                      NULL : 
                                      xstrdup("./memcards/card1.mcd");

  g_settings.audio_backend = AUDIO_BACKEND_SDL;

  /* Fast boot currently trips Crash into a black-screen path after it loads a
   * few offscreen textures. Prefer correctness for commercial discs; keep the
   * BIOS patch available for targeted bringup with CUPID_FASTBOOT=1. */

  /* Auto-precache CHD images: chd_read() under the worker thread serializes
   * disc I/O via mutex hold; even a 1ms hunk decode amplifies into measurable
   * emulator slowdown when the game streams CDDA / FMV.  RE2 went from 73% to
   * 98% speed in side-by-side testing.  Suppress with --no-cdrom-precache.
   * .m3u also auto-precaches: cd_image_precache_m3u proxies to current_image,
   * so a CHD-backed playlist gets precached too.  cue/bin .m3u silently skip. */
  bool path_auto_precache = false;
  if (s_fe.game_path) {
    const size_t n = strlen(s_fe.game_path);
    if (n >= 4 && (strcasecmp(s_fe.game_path + n - 4, ".chd") == 0 ||
                   strcasecmp(s_fe.game_path + n - 4, ".m3u") == 0))
      path_auto_precache = true;
  }
  g_settings.cdrom_load_image_to_ram = s_fe.cdrom_precache || path_auto_precache;

  g_settings.bios_patch_fast_boot = false;
  const char* fastboot = getenv("CUPID_FASTBOOT");
  if (fastboot && strcmp(fastboot, "1") == 0)
    g_settings.bios_patch_fast_boot = true;

  /* CUPID_OLD_MDEC=1 swaps the new (Mednafen integer) decoder for the old
   * (float reference) one. Diagnostic toggle for FMV corruption; if old
   * path renders correctly and new path is rainbow-noisy, the bug is in
   * mdec_decode_rle_new / mdec_idct_new / mdec_yuv_to_rgb_new. */
  if (getenv("CUPID_OLD_MDEC"))
    g_settings.mdec_use_old_routines = true;

  /* CUPID_NO_CHROMA_SMOOTH=1 disables 24-bit FMV chroma smoothing.
   * Diagnostic for FMV horizontal-banding artifacts. */
  if (getenv("CUPID_NO_CHROMA_SMOOTH"))
    g_settings.display_24bit_chroma_smoothing = false;

  /* Mirror to the GPU thread's copy (we don't actually use a GPU thread
   * but core code reads from g_gpu_settings in places). */
  g_gpu_settings = g_settings;
  /* Re-strdup any strings that need independent ownership. */
  if (g_settings.memory_card_paths[0])
    g_gpu_settings.memory_card_paths[0] = xstrdup(g_settings.memory_card_paths[0]);
}

/* Toggle vsync to track fast-forward state.  With vsync on, the present rate
 * is capped at the display refresh, so fast-forward (target_speed=0) cannot
 * actually run faster than 60/144/whatever-Hz no matter what the throttler
 * reports.  Drop vsync while Tab is held; restore on release. */
static void apply_vsync_for_fast_forward(bool ff_active)
{
  if (g_settings.gpu_renderer == GPU_RENDERER_HARDWARE_OPENGL) {
    opengl_context_t* ctx = opengl_device_get_context();
    if (!ctx) return;
    Error e = ERROR_INIT;
    opengl_swap_chain_set_swap_interval(ctx,
                                        ff_active ? GPU_VSYNC_MODE_DISABLED : GPU_VSYNC_MODE_FIFO,
                                        &e);
    Error_destroy(&e);
  } else if (s_fe.renderer) {
    /* SDL 2.0.18+; falls through silently on older if the symbol is unavailable. */
    SDL_RenderSetVSync(s_fe.renderer, ff_active ? 0 : 1);
  }
}

/* Try to bring up the HW OpenGL backend after SDL window is created.  Returns
 * true on success (g_gpu_backend set to HW); false means caller should fall
 * back to SW.  Logs to stderr on any step that fails.
 *
 * Uses WINDOW_INFO_TYPE_XLIB so the EGL surface binds to the SDL window's X11
 * drawable.  Frontend present moves to gl_present_frame (fullscreen-quad blit
 * of display_buffer) instead of SDL_Renderer, so there's no GL/GLX contention. */
static bool try_init_hw_opengl_backend(void)
{
  SDL_SysWMinfo wm;
  SDL_VERSION(&wm.version);
  if (!SDL_GetWindowWMInfo(s_fe.window, &wm)) {
    fprintf(stderr, "[gpu] SDL_GetWindowWMInfo failed: %s\n", SDL_GetError());
    return false;
  }
  if (wm.subsystem != SDL_SYSWM_X11) {
    fprintf(stderr, "[gpu] HW OpenGL backend needs X11 (subsystem=%d).\n", (int)wm.subsystem);
    return false;
  }
  window_info_t wi; window_info_init(&wi);
  wi.type               = WINDOW_INFO_TYPE_XLIB;
  wi.display_connection = wm.info.x11.display;
  wi.window_handle      = (void*)(uintptr_t)wm.info.x11.window;
  int win_w = 0, win_h = 0;
  SDL_GetWindowSize(s_fe.window, &win_w, &win_h);
  wi.surface_width      = (u16)win_w;
  wi.surface_height     = (u16)win_h;
  wi.surface_format     = GPU_TEXTURE_FORMAT_RGBA8;
  wi.surface_scale      = 1.0f;
  wi.surface_refresh_rate = 60.0f;

  Error e = ERROR_INIT;
  if (!opengl_device_create(&wi, GPU_VSYNC_MODE_FIFO, GPU_DEVICE_CREATE_NONE, &e)) {
    fprintf(stderr, "[gpu] opengl_device_create failed: %s\n", Error_get_description(&e));
    Error_destroy(&e);
    return false;
  }
  Error_destroy(&e);

  Error e2 = ERROR_INIT;
  gpu_backend_t* be = gpu_backend_create_hardware_opengl(&wi, &e2);
  if (!be) {
    fprintf(stderr, "[gpu] gpu_backend_create_hardware_opengl failed: %s\n",
            Error_get_description(&e2));
    Error_destroy(&e2);
    opengl_device_destroy();
    return false;
  }
  Error_destroy(&e2);
  /* gpu_backend_create_hardware_opengl already publishes via g_gpu_backend. */
  return true;
}

static bool sdl_setup(void)
{
  /* Stop SDL from hijacking SIGINT so our handler still runs on ^C. */
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return false;
  }
  /* Re-install in case SDL still touched them. */
  {
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  /* no SA_RESTART; want syscalls to EINTR so loop unwinds */
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
  }

  Uint32 win_flags = SDL_WINDOW_RESIZABLE;
  if (g_settings.gpu_renderer == GPU_RENDERER_HARDWARE_OPENGL)
    win_flags |= SDL_WINDOW_OPENGL;

  s_fe.window = SDL_CreateWindow("cupid-ps1",
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 800, 600,
                                 win_flags);
  /* If SDL refuses an OpenGL window (no GL on this driver, e.g. dummy in
   * headless tests), retry without the GL flag and revert the renderer to
   * software so the rest of init takes the SW path. */
  if (!s_fe.window && (win_flags & SDL_WINDOW_OPENGL)) {
    fprintf(stderr, "[gpu] SDL_CreateWindow with OpenGL failed (%s); "
                    "retrying without GL.\n", SDL_GetError());
    win_flags &= ~(Uint32)SDL_WINDOW_OPENGL;
    g_settings.gpu_renderer     = GPU_RENDERER_SOFTWARE;
    g_gpu_settings.gpu_renderer = GPU_RENDERER_SOFTWARE;
    s_fe.window = SDL_CreateWindow("cupid-ps1",
                                   SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                   800, 600,
                                   win_flags);
  }
  if (!s_fe.window) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    return false;
  }

  /* HW path: try to bring up GL device + HW backend.  On any failure, revert
   * to SW so the runtime stays bootable. */
  if (g_settings.gpu_renderer == GPU_RENDERER_HARDWARE_OPENGL) {
    if (try_init_hw_opengl_backend()) {
      /* HW: present via gl_present_frame; no SDL_Renderer needed. */
      s_fe.renderer = NULL;
      return true;
    }
    fprintf(stderr, "[gpu] HW OpenGL setup failed - falling back to software.\n");
    g_settings.gpu_renderer     = GPU_RENDERER_SOFTWARE;
    g_gpu_settings.gpu_renderer = GPU_RENDERER_SOFTWARE;
  }

  /* SW path: create SDL_Renderer.  When falling back from HW (window has
   * SDL_WINDOW_OPENGL), force SOFTWARE so SDL's renderer doesn't try to use
   * GL on a drawable our EGL had touched. */
  const Uint32 r_flags = (win_flags & SDL_WINDOW_OPENGL)
                          ? SDL_RENDERER_SOFTWARE
                          : (SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  s_fe.renderer = SDL_CreateRenderer(s_fe.window, -1, r_flags);
  if (!s_fe.renderer) {
    s_fe.renderer = SDL_CreateRenderer(s_fe.window, -1, SDL_RENDERER_SOFTWARE);
    if (!s_fe.renderer) {
      fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
      return false;
    }
  }

  /* Texture: created lazily on the first frame so we know the size. */
  s_fe.texture        = NULL;
  s_fe.texture_width  = 0;
  s_fe.texture_height = 0;

  input_map_open_all_gamepads();

  /* Seed initial window size for normalized lightgun coordinates. */
  {
    int ww = 0, wh = 0;
    SDL_GetWindowSize(s_fe.window, &ww, &wh);
    input_map_set_window_size((s32)ww, (s32)wh);
  }
  return true;
}

static bool ensure_texture(u32 w, u32 h)
{
  if (s_fe.texture && s_fe.texture_width == w && s_fe.texture_height == h)
    return true;
  if (s_fe.texture) {
    SDL_DestroyTexture(s_fe.texture);
    s_fe.texture = NULL;
  }
  if (w == 0 || h == 0)
    return false;

  /* gpu_sw writes u32 with byte order R,G,B,A.  On little-endian that's
   * SDL_PIXELFORMAT_ABGR8888 (numerically 0xAABBGGRR; "ABGR" naming is
   * the byte order from MSB-down). */
  s_fe.texture = SDL_CreateTexture(s_fe.renderer,
                                   SDL_PIXELFORMAT_ABGR8888,
                                   SDL_TEXTUREACCESS_STREAMING,
                                   (int)w, (int)h);
  if (!s_fe.texture) {
    fprintf(stderr, "SDL_CreateTexture(%u,%u) failed: %s\n", w, h, SDL_GetError());
    return false;
  }
  s_fe.texture_width  = w;
  s_fe.texture_height = h;
  return true;
}

static void compute_letterbox(u32 fb_w, u32 fb_h, int win_w, int win_h, SDL_Rect* out)
{
  if (fb_w == 0 || fb_h == 0 || win_w <= 0 || win_h <= 0) {
    out->x = 0; out->y = 0; out->w = win_w; out->h = win_h;
    return;
  }
  const float fb_aspect  = (float)fb_w / (float)fb_h;
  const float win_aspect = (float)win_w / (float)win_h;
  if (win_aspect > fb_aspect) {
    /* window wider than fb: pillarbox. */
    out->h = win_h;
    out->w = (int)((float)win_h * fb_aspect + 0.5f);
    out->x = (win_w - out->w) / 2;
    out->y = 0;
  } else {
    out->w = win_w;
    out->h = (int)((float)win_w / fb_aspect + 0.5f);
    out->x = 0;
    out->y = (win_h - out->h) / 2;
  }
}

static struct {
  GLuint program;
  GLuint vao;
  GLuint texture;
  u32    tex_w;
  u32    tex_h;
  bool   inited;
  bool   broken;   /* compile/link failed; degrade to glClear-only */
} s_gl_present;

static GLuint gl_compile_shader_inline(GLenum stage, const char* src)
{
  GLuint sh = glCreateShader(stage);
  glShaderSource(sh, 1, &src, NULL);
  glCompileShader(sh);
  GLint status = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &status);
  if (!status) {
    char log[1024];
    GLint len = 0;
    glGetShaderInfoLog(sh, (GLsizei)sizeof(log), &len, log);
    fprintf(stderr, "[gl_present] shader compile failed: %.*s\n", (int)len, log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

static bool gl_present_init(void)
{
  static const char* vs_src =
    "#version 330 core\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 uv = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  v_uv = uv;\n"
    "  gl_Position = vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);\n" 
    "}\n";
  static const char* fs_src =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_col;\n" 
    "void main() { o_col = texture(u_tex, v_uv); }\n";

  GLuint vs = gl_compile_shader_inline(GL_VERTEX_SHADER, vs_src);
  GLuint fs = gl_compile_shader_inline(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) { s_gl_present.broken = true; return false; }
  s_gl_present.program = glCreateProgram();
  glAttachShader(s_gl_present.program, vs);
  glAttachShader(s_gl_present.program, fs);
  glLinkProgram(s_gl_present.program);
  GLint linked = 0;
  glGetProgramiv(s_gl_present.program, GL_LINK_STATUS, &linked);
  glDeleteShader(vs);
  glDeleteShader(fs);
  if (!linked) {
    char log[1024];
    GLint len = 0;
    glGetProgramInfoLog(s_gl_present.program, (GLsizei)sizeof(log), &len, log);
    fprintf(stderr, "[gl_present] program link failed: %.*s\n", (int)len, log);
    glDeleteProgram(s_gl_present.program);
    s_gl_present.program = 0;
    s_gl_present.broken = true;
    return false;
  }
  glGenVertexArrays(1, &s_gl_present.vao);
  glGenTextures(1, &s_gl_present.texture);
  s_gl_present.inited = true;
  return true;
}

static void gl_present_frame(const u32* pixels, u32 w, u32 h)
{
  if (!s_gl_present.inited && !s_gl_present.broken) gl_present_init();

  static u32 dbg_present_n = 0;
  if (getenv("CUPID_TRACE") && (dbg_present_n++ & 0x3F) == 0) {
    u32 nz = 0;
    if (pixels && w && h) {
      const u32 total = w * h;
      for (u32 i = 0; i < total; i++) if (pixels[i] != 0xFF000000u && pixels[i] != 0) nz++;
    }
    fprintf(stderr, "[gl_present #%u] %ux%u nz=%u\n", dbg_present_n, w, h, nz);
  }

  int win_w = 0, win_h = 0;
  int dw = 0, dh = 0, lw = 0, lh = 0;
  SDL_GL_GetDrawableSize(s_fe.window, &dw, &dh);
  SDL_GetWindowSize(s_fe.window, &lw, &lh);
  win_w = dw; win_h = dh;
  if (win_w <= 0 || win_h <= 0) { win_w = lw; win_h = lh; }
  if (win_w <= 0 || win_h <= 0) return;
  if (getenv("CUPID_TRACE") && (dbg_present_n & 0x3F) == 1) {
    fprintf(stderr, "[gl_present sizes] drawable=%dx%d window=%dx%d\n", dw, dh, lw, lh);
  }

  /* Leaves GL bound to the device's last vram_texture FBO from the most
   * recent hw_*_gl draw.  Bind the default framebuffer (the EGL window
   * surface) so glClear + glDrawArrays target the swap chain, not vram.
   *
   * Also reset the per-pipeline GL state that the device caches but doesn't
   * own here; scissor (would clip our fullscreen quad to the last polygon's
   * drawing-area, manifesting as "only the bottom-left corner renders"),
   * blend (could blend our texture with garbage), color mask (could disable
   * channel writes), depth/cull (defensive). Without disabling scissor, glClear
   * itself only clears the scissor rect, leaving the rest of the window
   * displaying stale framebuffer contents. */
  const GLuint saved_fbo = opengl_device_get_current_fbo();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_FALSE);

  glViewport(0, 0, win_w, win_h);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (!s_gl_present.broken && pixels && w > 0 && h > 0) {
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_gl_present.texture);
    if (w != s_gl_present.tex_w || h != s_gl_present.tex_h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)w, (GLsizei)h,
                   0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
      s_gl_present.tex_w = w; s_gl_present.tex_h = h;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)w, (GLsizei)h,
                      GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* Anamorphic 4:3 letterbox via viewport (matches SW's compute_letterbox). */
    const u32 logical_w = (h * 4u) / 3u;
    const u32 logical_h = h;
    const float fb_aspect  = (float)logical_w / (float)logical_h;
    const float win_aspect = (float)win_w / (float)win_h;
    int rx, ry, rw, rh;
    if (win_aspect > fb_aspect) {
      rh = win_h;
      rw = (int)((float)win_h * fb_aspect + 0.5f);
      rx = (win_w - rw) / 2;
      ry = 0;
    } else {
      rw = win_w;
      rh = (int)((float)win_w / fb_aspect + 0.5f);
      rx = 0;
      ry = (win_h - rh) / 2;
    }
    glViewport(rx, ry, rw, rh);
    if (getenv("CUPID_TRACE") && (dbg_present_n & 0x3F) == 1) {
      fprintf(stderr, "[gl_present] win=%dx%d vp=(%d,%d %dx%d) tex=%ux%u logical=%ux%u\n",
              win_w, win_h, rx, ry, rw, rh, s_gl_present.tex_w, s_gl_present.tex_h, logical_w, logical_h);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glUseProgram(s_gl_present.program);
    glBindVertexArray(s_gl_present.vao);
    GLint loc = glGetUniformLocation(s_gl_present.program, "u_tex");
    if (loc >= 0) glUniform1i(loc, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
  }

  opengl_context_swap_buffers(opengl_device_get_context());

  /* Restore the FBO opengl_device thinks is bound, so its set_render_targets
   * cache stays consistent on the next hw_*_gl draw.  Re-enable scissor test
   * because the device only updates the scissor *box* via opengl_device_set_scissor;
   * it assumes GL_SCISSOR_TEST is enabled (set once at create time, see
   * opengl_device.c:463 and render_blank_frame_internal:343).  Without this
   * re-enable, every subsequent batch draws without scissor.
   *
   * Also invalidate the device's pipeline-state caches: we touched glDisable
   * BLEND/DEPTH/CULL/STENCIL and glColorMask, but the device caches what it
   * thinks GL is in and skips re-issuing equal state.  Without invalidation,
   * the next batch bind sees its cached state still matches and won't restore
   * GL.  Setting caches to 0xFF (the create-time sentinel) forces a full
   * state re-issue on the next pipeline bind.
   *
   * Pipeline / VAO / program caches: this presenter unbinds VAO+program at
   * lines above (`glBindVertexArray(0); glUseProgram(0);`).  If we don't
   * invalidate the device caches, the *next* opengl_device_set_pipeline()
   * call early-returns when the requested pipeline matches the cached one,
   * leaving GL with nothing bound; glDrawArrays raises
   * GL_INVALID_OPERATION on the second-and-subsequent extract draws (the
   * first one is fine because flush_render's batch pipeline gets bound
   * before extract_display picks its own). */
  glBindFramebuffer(GL_FRAMEBUFFER, saved_fbo);
  glEnable(GL_SCISSOR_TEST);
  opengl_device_invalidate_state_cache();
}

static void sdl_present_frame(void)
{
  u32 w = 0, h = 0;
  const u32* pixels = (g_settings.gpu_renderer == GPU_RENDERER_HARDWARE_OPENGL)
                        ? gpu_hw_get_display_buffer(&w, &h)
                        : gpu_sw_get_display_buffer(&w, &h);

  /* One-shot PPM dump for visual verification (works for both HW and SW). */
  {
    const char* dump_path = getenv("CUPID_DUMP_PPM");
    static u32 dump_n = 0;
    const char* dump_at = getenv("CUPID_DUMP_AT");
    const u32 trigger_at = dump_at ? (u32)atoi(dump_at) : 600u;
    if (dump_path && pixels && w && h) {
      if (dump_n == trigger_at) {
        FILE* f = fopen(dump_path, "wb");
        if (f) {
          fprintf(f, "P6\n%u %u\n255\n", w, h);
          for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
              const u32 px = pixels[y * w + x];
              u8 rgb[3] = { (u8)(px & 0xFF), (u8)((px >> 8) & 0xFF), (u8)((px >> 16) & 0xFF) };
              fwrite(rgb, 1, 3, f);
            }
          }
          fclose(f);
          fprintf(stderr, "[ppm dump] wrote %s (%ux%u) at frame %u (renderer=%d)\n", dump_path, w, h, dump_n, (int)g_settings.gpu_renderer);
        }
        /* Also dump g_vram as PPM for cross-backend g_vram diff. */
        const char* vram_path = getenv("CUPID_DUMP_VRAM_PPM");
        if (vram_path) {
          extern u16 g_vram[];
          FILE* vf = fopen(vram_path, "wb");
          if (vf) {
            fprintf(vf, "P6\n%d %d\n255\n", 1024, 512);
            for (int yy = 0; yy < 512; yy++) {
              for (int xx = 0; xx < 1024; xx++) {
                const u16 v = g_vram[yy * 1024 + xx];
                u8 r = (u8)((v & 0x1F) << 3);
                u8 g = (u8)(((v >> 5) & 0x1F) << 3);
                u8 b = (u8)(((v >> 10) & 0x1F) << 3);
                u8 rgb[3] = { r, g, b };
                fwrite(rgb, 1, 3, vf);
              }
            }
            fclose(vf);
            fprintf(stderr, "[vram dump] wrote %s\n", vram_path);
          }
        }
      }
      dump_n++;
    }
  }

  /* HW path: bypass SDL_Renderer entirely (no s_fe.renderer in HW mode), use
   * GL fullscreen-quad blit + EGL swap. */
  if (g_settings.gpu_renderer == GPU_RENDERER_HARDWARE_OPENGL && !s_fe.renderer) {
    gl_present_frame(pixels, w, h);
    return;
  }

  static u32 dbg_count = 0;
  if (getenv("CUPID_TRACE")) {
    if ((dbg_count++ & 0x3F) == 0) {
      u32 nz = 0;
      if (pixels && w && h) {
        const u32 total = w * h;
        for (u32 i = 0; i < total; i++) if (pixels[i] != 0) nz++;
      }
      extern u16 g_vram[];
      u32 vram_nz = 0;
      const u32 VRAM_W = 1024, VRAM_H = 512;
      for (u32 i = 0; i < VRAM_W * VRAM_H; i++) if (g_vram[i] != 0) vram_nz++;
      const gpu_backend_counters_t* c = g_gpu_backend ? &g_gpu_backend->counters : NULL;
      const u32* op_counts = gpu_dbg_op_counts();
      u32 op_top[4] = {0,0,0,0};
      u32 op_idx[4] = {0,0,0,0};
      for (int o = 0; o < 256; o++) {
        u32 v = op_counts[o];
        for (int s = 0; s < 4; s++) {
          if (v > op_top[s]) {
            for (int t = 3; t > s; t--) { op_top[t] = op_top[t-1]; op_idx[t] = op_idx[t-1]; }
            op_top[s] = v; op_idx[s] = (u32)o; break;
          }
        }
      }
      const u32* irqs = irq_dbg_set_counts();
      const u32* dmas = dma_dbg_transfer_counts();
      const u32* gte_counts = gte_dbg_op_counts();
      u32 gte_top[4] = {0,0,0,0};
      u32 gte_idx[4] = {0,0,0,0};
      for (int o = 0; o < 64; o++) {
        u32 v = gte_counts[o];
        for (int s = 0; s < 4; s++) {
          if (v > gte_top[s]) {
            for (int t = 3; t > s; t--) { gte_top[t] = gte_top[t-1]; gte_idx[t] = gte_idx[t-1]; }
            gte_top[s] = v; gte_idx[s] = (u32)o; break;
          }
        }
      }
      fprintf(stderr, "[present #%u] %ux%u pc=%08x npc=%08x vram_nz=%u  prims=%u writes=%u  gpu-ops: %02x:%u %02x:%u %02x:%u %02x:%u  gte-ops: %02x:%u %02x:%u %02x:%u %02x:%u\n"
                      "                irqs vbl=%u gpu=%u cdr=%u dma=%u tim0=%u tim1=%u tim2=%u pad=%u sio=%u spu=%u irq10=%u status=%03x mask=%03x cpu_dispatch=%u\n"
                      "                dma mdec_in=%u mdec_out=%u gpu=%u cdr=%u spu=%u otc=%u  cd drv=%u lba=%u req=%u buf=%08x cmd=%08x  mdec st=%08x fifo=%08x\n"
                      "                dma2 madr=%06x bcr=%08x chcr=%08x req=%u dicr=%08x  timers t0=%04x/%04x/%04x t1=%04x/%04x/%04x t2=%04x/%04x/%04x\n",
              dbg_count, w, h, g_cpu_state.pc, g_cpu_state.npc, vram_nz,
              c ? c->num_primitives : 0, c ? c->num_writes : 0,
              op_idx[0], op_top[0], op_idx[1], op_top[1],
              op_idx[2], op_top[2], op_idx[3], op_top[3],
              gte_idx[0], gte_top[0], gte_idx[1], gte_top[1],
              gte_idx[2], gte_top[2], gte_idx[3], gte_top[3],
              irqs[0], irqs[1], irqs[2], irqs[3], irqs[4], irqs[5], irqs[6], irqs[7],
              irqs[8], irqs[9], irqs[10], irq_dbg_status_register(), irq_dbg_mask_register(), 
              cpu_dbg_irq_dispatch_count(),
              dmas[0], dmas[1], dmas[2], dmas[3], dmas[4], dmas[6],
              cdrom_dbg_drive_state(), cdrom_dbg_current_lba(), cdrom_dbg_requested_lba(),
              cdrom_dbg_buffer_status(), cdrom_dbg_command_state(), mdec_dbg_status(), mdec_dbg_fifo_status(),
              dma_dbg_base_address(DMA_CHANNEL_GPU), dma_dbg_block_control(DMA_CHANNEL_GPU),
              dma_dbg_channel_control(DMA_CHANNEL_GPU), dma_dbg_request(DMA_CHANNEL_GPU), dma_dbg_dicr(),
              timers_dbg_counter(0), timers_dbg_mode(0), timers_dbg_target(0),
              timers_dbg_counter(1), timers_dbg_mode(1), timers_dbg_target(1),
              timers_dbg_counter(2), timers_dbg_mode(2), timers_dbg_target(2));
      trace_task_state();
      trace_pc_window();
      (void)nz;
    }
  }

  SDL_SetRenderDrawColor(s_fe.renderer, 0, 0, 0, 255);
  SDL_RenderClear(s_fe.renderer);

  if (pixels && w > 0 && h > 0 && ensure_texture(w, h)) {
    if (SDL_UpdateTexture(s_fe.texture, NULL, pixels, (int)(w * sizeof(u32))) == 0) {
      int win_w = 0, win_h = 0;
      SDL_GetRendererOutputSize(s_fe.renderer, &win_w, &win_h);
      if (getenv("CUPID_TRACE") && (dbg_count & 0x3F) == 1)
        fprintf(stderr, "[sdl_present] renderer_output=%dx%d\n", win_w, win_h);
      /* PS1 always outputs to a 4:3 TV.  The active-raster framebuffer
       * (e.g. 700x480 or 320x240) is anamorphic; apply 4:3 logical aspect
       * for the letterbox calc so SDL_RenderCopy squashes/stretches the
       * source texture to fit.  Without this, 700x480 (~1.46) gets visible
       * top/bottom letterbox instead of filling a 4:3 window edge-to-edge. */
      const u32 logical_w = (h * 4u) / 3u;
      const u32 logical_h = h;
      SDL_Rect dst;
      compute_letterbox(logical_w, logical_h, win_w, win_h, &dst);
      SDL_RenderCopy(s_fe.renderer, s_fe.texture, NULL, &dst);
    }
  }

  SDL_RenderPresent(s_fe.renderer);
}

static void trace_pc_window(void)
{
  if (!getenv("CUPID_TRACE_PC"))
    return;

  const cpu_registers_t* r = &g_cpu_state.regs;
  fprintf(stderr,
          "[pc-window] pc=%08x npc=%08x sr=%08x cause=%08x epc=%08x "
          "a0=%08x a1=%08x a2=%08x a3=%08x v0=%08x v1=%08x "
          "s0=%08x s1=%08x s2=%08x s3=%08x s4=%08x s5=%08x ra=%08x sp=%08x\n",
          g_cpu_state.pc, g_cpu_state.npc, g_cpu_state.cop0_regs.sr.bits,
          g_cpu_state.cop0_regs.cause.bits, g_cpu_state.cop0_regs.EPC,
          r->named.a0, r->named.a1, r->named.a2, r->named.a3,
          r->named.v0, r->named.v1, r->named.s0, r->named.s1,
          r->named.s2, r->named.s3, r->named.s4, r->named.s5, 
          r->named.ra, r->named.sp);
  trace_disasm_window(g_cpu_state.pc, '>');
  if ((g_cpu_state.npc & ~0x3Fu) != (g_cpu_state.pc & ~0x3Fu))
    trace_disasm_window(g_cpu_state.npc, '*');
}

static void trace_task_entry(const char* label, u32 addr)
{
  u32 w00 = 0, w08 = 0, w0c = 0, w10 = 0, w14 = 0, w18 = 0, w1c = 0, w20 = 0, w24 = 0, w28 = 0;
  u16 h04 = 0, h06 = 0;
  const bool ok =
    cpu_safe_read_memory_word(addr + 0x00u, &w00) &&
    cpu_safe_read_memory_halfword(addr + 0x04u, &h04) &&
    cpu_safe_read_memory_halfword(addr + 0x06u, &h06) &&
    cpu_safe_read_memory_word(addr + 0x08u, &w08) &&
    cpu_safe_read_memory_word(addr + 0x0cu, &w0c) &&
    cpu_safe_read_memory_word(addr + 0x10u, &w10) &&
    cpu_safe_read_memory_word(addr + 0x14u, &w14) &&
    cpu_safe_read_memory_word(addr + 0x18u, &w18) &&
    cpu_safe_read_memory_word(addr + 0x1cu, &w1c) &&
    cpu_safe_read_memory_word(addr + 0x20u, &w20) &&
    cpu_safe_read_memory_word(addr + 0x24u, &w24) &&
    cpu_safe_read_memory_word(addr + 0x28u, &w28);
  if (!ok) {
    fprintf(stderr, "  task %s @%08x: <unreadable>\n", label, addr);
    return;
  }

  fprintf(stderr,
          "  task %s @%08x: +00=%08x st=%04x ty=%04x +08=%08x +0c=%08x +10=%08x "
          "+14=%08x +18=%08x +1c=%08x +20=%08x +24=%08x +28=%08x\n", 
          label, addr, w00, h04, h06, w08, w0c, w10, w14, w18, w1c, w20, w24, w28);
}

static void trace_task_state(void)
{
  if (!getenv("CUPID_TRACE_TASK"))
    return;

  const u32 pc = g_cpu_state.pc;
  if (pc < 0x80013500u || pc > 0x80014220u)
    return;

  const cpu_registers_t* r = &g_cpu_state.regs;
  u32 count = 0;
  bool have_count = cpu_safe_read_memory_word(r->named.s2 - 4u, &count);
  if (count > 128u)
    count = 128u;

  fprintf(stderr,
          "[task-state] pc=%08x npc=%08x a0=%08x s1=%08x s2=%08x count=%s%u "
          "s0=%08x s3=%08x s5=%08x t0=%08x t1=%08x t2=%08x v0=%08x v1=%08x\n",
          g_cpu_state.pc, g_cpu_state.npc, r->named.a0, r->named.s1, r->named.s2,
          have_count ? "" : "?", count, r->named.s0, r->named.s3, r->named.s5, 
          r->named.t0, r->named.t1, r->named.t2, r->named.v0, r->named.v1);

  trace_task_entry("a0", r->named.a0);
  if (r->named.s1 != r->named.a0)
    trace_task_entry("s1", r->named.s1);
  if (r->named.s2 != r->named.a0 && r->named.s2 != r->named.s1)
    trace_task_entry("s2", r->named.s2);

  if (!have_count || r->named.s2 == 0)
    return;

  for (u32 i = 0; i < count; i++) {
    const u32 entry = r->named.s2 + i * 44u;
    if (entry == r->named.a0 || entry == r->named.s1)
      continue;

    u16 state = 0;
    if (!cpu_safe_read_memory_halfword(entry + 4u, &state))
      continue;
    if (state == 0)
      continue;

    char label[16];
    snprintf(label, sizeof(label), "#%u", i);
    trace_task_entry(label, entry);
  }
}

static void trace_disasm_window(u32 center, char marker)
{
  const u32 start = (center >= 16u) ? (center - 16u) : center;
  for (u32 addr = start; addr < start + 36u; addr += 4u) {
    u32 instr = 0;
    if (!cpu_safe_read_memory_word(addr, &instr)) {
      fprintf(stderr, "  %08x: ???????? <unreadable>\n", addr);
      continue;
    }

    small_string_stack_t text;
    small_string_stack_init(&text);
    cpu_disassemble(&text.s, instr, addr);
    fprintf(stderr, "%c %08x: %08x  %s\n",
            (addr == center) ? marker : ' ', addr, instr, small_string_c_str(&text.s));
    small_string_destroy(&text.s);
  }
}

static void sdl_teardown(void)
{
  input_map_close_all_gamepads();
  if (s_fe.texture)  SDL_DestroyTexture(s_fe.texture);
  if (s_fe.renderer) SDL_DestroyRenderer(s_fe.renderer);
  if (s_fe.window)   SDL_DestroyWindow(s_fe.window);
  s_fe.texture  = NULL;
  s_fe.renderer = NULL;
  s_fe.window   = NULL;
  SDL_Quit();
}

static bool audio_setup(void)
{
  Error e; Error_init(&e);

  s_fe.audio = core_audio_stream_create();
  if (!s_fe.audio) {
    fprintf(stderr, "core_audio_stream_create failed (out of memory)\n");
    Error_destroy(&e);
    return false;
  }

  const settings_audio_stream_parameters_t* ap = &g_settings.audio_stream_parameters;
  if (!core_audio_stream_initialize(s_fe.audio,
                                    g_settings.audio_backend,
                                    /*sample_rate*/ 44100u,
                                    ap,
                                    /*driver_name*/ NULL,
                                    /*device_name*/ NULL,
                                    &e)) {
    fprintf(stderr, "core_audio_stream_initialize failed: %s\n", Error_get_description(&e));
    core_audio_stream_destroy(s_fe.audio);
    s_fe.audio = NULL;
    Error_destroy(&e);
    return false;
  }
  Error_destroy(&e);

  spu_set_output_stream(s_fe.audio);
  return true;
}

static void audio_teardown(void)
{
  spu_set_output_stream(NULL);
  if (s_fe.audio) {
    core_audio_stream_destroy(s_fe.audio);
    s_fe.audio = NULL;
  }
}

static void test_savestate_roundtrip_tick(void)
{
  if (!s_fe.test_rt_enabled || !system_is_valid()) return;

  const u32 frame = system_get_frame_number();
  if (!s_fe.test_rt_saved && frame >= 300u) {
    /* CUPID_RT_PATH overrides the default `.sav` path so we can verify
     * gzip / zstd / xz outer wrappers via the same harness. */
    const char* env_path = getenv("CUPID_RT_PATH");
    const char* path = (env_path && env_path[0]) ? env_path : "/tmp/cupid-ps1-rt-test.sav";
    Error e; Error_init(&e);
    if (!system_save_state(path, &e, false, true)) {
      fprintf(stderr, "[rt-test] save_state failed: %s\n", Error_get_description(&e));
      Error_destroy(&e);
      _exit(1);
    }
    Error_destroy(&e);

    Error_init(&e);
    if (system_load_state(path, &e, false, false) == 0) {
      fprintf(stderr, "[rt-test] load_state failed: %s\n", Error_get_description(&e));
      Error_destroy(&e);
      _exit(1);
    }
    Error_destroy(&e);
    fprintf(stderr, "[rt-test] save+load round-trip OK at frame %u\n", frame);
    s_fe.test_rt_saved = true;
    s_fe.test_rt_quit_frame = system_get_frame_number() + 60u;
  }

  if (s_fe.test_rt_saved && frame >= s_fe.test_rt_quit_frame) {
    fprintf(stderr, "[rt-test] PASS - ran 60 frames after load, exiting clean\n");
    s_fe.quit_requested = true;
    system_interrupt_execution();
  }
}

void host_pump_messages_on_core_thread(void)
{
  test_savestate_roundtrip_tick();

  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_QUIT:
        s_fe.quit_requested = true;
        system_interrupt_execution();
        break;

      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_CLOSE) {
          s_fe.quit_requested = true;
          system_interrupt_execution();
        } else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                   ev.window.event == SDL_WINDOWEVENT_RESIZED) {
          input_map_set_window_size(ev.window.data1, ev.window.data2);
        }
        break;

      case SDL_MOUSEMOTION:
        input_map_handle_mouse_motion(ev.motion.x, ev.motion.y,
                                      ev.motion.xrel, ev.motion.yrel);
        break;

      case SDL_MOUSEBUTTONDOWN:
        input_map_handle_mouse_button(ev.button.button, true);
        break;
      case SDL_MOUSEBUTTONUP:
        input_map_handle_mouse_button(ev.button.button, false);
        break;

      case SDL_KEYDOWN:
        input_map_handle_key_event(&ev.key);
        if (!ev.key.repeat) {
          const Uint16 mod = ev.key.keysym.mod;
          const bool   alt_down   = (mod & KMOD_ALT)   != 0;
          const bool   shift_down = (mod & KMOD_SHIFT) != 0;
          switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE:
              s_fe.quit_requested = true;
              system_interrupt_execution();
              break;
            case SDL_SCANCODE_F1: {
              /* Analog mode toggle on the pad. */
              controller_t* c = pad_get_controller(0);
              if (c) {
                controller_set_bind_state(c, (u32)ANALOG_CONTROLLER_BUTTON_ANALOG, 1.0f);
                controller_set_bind_state(c, (u32)ANALOG_CONTROLLER_BUTTON_ANALOG, 0.0f);
              }
              break;
            }
            case SDL_SCANCODE_F3:    /* previous sub-image (multi-disc m3u) */
            case SDL_SCANCODE_F4: {  /* next sub-image */
              if (system_has_media_sub_images()) {
                const u32 cnt = system_get_media_sub_image_count();
                const u32 cur = system_get_media_sub_image_index();
                u32 next = cur;
                if (ev.key.keysym.scancode == SDL_SCANCODE_F4) {
                  if (cur + 1u < cnt) next = cur + 1u;
                } else {
                  if (cur > 0u) next = cur - 1u;
                }
                if (next != cur) {
                  Error e; Error_init(&e);
                  if (!system_switch_media_sub_image(next, &e)) {
                    fprintf(stderr, "switch sub-image failed: %s\n", Error_get_description(&e));
                  }
                  Error_destroy(&e);
                }
              }
              break;
            }
            /* P: pause / resume. */
            case SDL_SCANCODE_P:
              if (system_is_valid())
                system_pause_system(!system_is_paused());
              break;
            /* F5: hard reset. */
            case SDL_SCANCODE_F5:
              if (system_is_valid())
                system_reset_system();
              break;
            /* Alt+Enter: fullscreen toggle.  Bare Return is still pad START
             * (input_map handles it via the keyboard scancode array). */
            case SDL_SCANCODE_RETURN:
              if (alt_down && s_fe.window) {
                s_fe.fullscreen = !s_fe.fullscreen;
                SDL_SetWindowFullscreen(s_fe.window,
                                        s_fe.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
              }
              break;
            /* Tab held: fast-forward.  Released in the SDL_KEYUP handler.
             * Drop vsync while held so the speed limiter can actually run
             * the present loop faster than the display refresh. */
            case SDL_SCANCODE_TAB:
              if (!s_fe.fast_forward_held) {
                s_fe.fast_forward_held = true;
                system_set_fast_forward_enabled(true);
                apply_vsync_for_fast_forward(true);
              }
              break;
            /* Shift+1..9: pick the active save-state slot for F11/F12. */
            case SDL_SCANCODE_1: case SDL_SCANCODE_2: case SDL_SCANCODE_3:
            case SDL_SCANCODE_4: case SDL_SCANCODE_5: case SDL_SCANCODE_6:
            case SDL_SCANCODE_7: case SDL_SCANCODE_8: case SDL_SCANCODE_9: {
              if (shift_down) {
                const s32 slot = (s32)(ev.key.keysym.scancode - SDL_SCANCODE_1) + 1;
                s_fe.current_save_slot = slot;
                fprintf(stderr, "[hotkey] save-state slot = %d\n", slot);
              }
              break;
            }
            case SDL_SCANCODE_F11: {
              const s32 slot = s_fe.current_save_slot ? s_fe.current_save_slot : 1;
              fprintf(stderr, "[hotkey] save state -> slot %d\n", (int)slot);
              system_save_state_to_slot(true, slot);
              break;
            }
            case SDL_SCANCODE_F12: {
              const s32 slot = s_fe.current_save_slot ? s_fe.current_save_slot : 1;
              fprintf(stderr, "[hotkey] load state <- slot %d\n", (int)slot);
              system_load_state_from_slot(true, slot);
              break;
            }
            default: break;
          }
        }
        break;

      case SDL_KEYUP:
        input_map_handle_key_event(&ev.key);
        if (ev.key.keysym.scancode == SDL_SCANCODE_TAB && s_fe.fast_forward_held) {
          s_fe.fast_forward_held = false;
          system_set_fast_forward_enabled(false);
          apply_vsync_for_fast_forward(false);
        }
        break;

      case SDL_CONTROLLERDEVICEADDED:
        input_map_on_controller_added((s32)ev.cdevice.which);
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        input_map_on_controller_removed(ev.cdevice.which);
        break;

      default: break;
    }
  }

  /* Update the framebuffer presentation. */
  sdl_present_frame();
}

void host_input_manager_poll_sources(void)
{
  /* Order: keyboard sets baseline, gamepads OR-merge in. */
  input_map_apply_keyboard();
  input_map_apply_gamepads();
}

void host_wait_for_all_async_tasks(void)
{
  /* Frontend doesn't run async tasks today.  When the task queue lands,
   * this needs to drain it. */
}

static bool boot_and_run(void)
{
  Error e; Error_init(&e);

  if (!system_process_startup(&e)) {
    fprintf(stderr, "system_process_startup failed: %s\n", Error_get_description(&e));
    Error_destroy(&e);
    return false;
  }
  /* Init exec trace BEFORE first JIT block compile so the prologue
   * emit decision sees the env var. */
  cpu_code_cache_dbg_init_exec_trace();
  if (!system_core_thread_initialize(&e)) {
    fprintf(stderr, "system_core_thread_initialize failed: %s\n",
            Error_get_description(&e));
    Error_destroy(&e);
    return false;
  }

  system_boot_parameters_t p;
  system_boot_parameters_init(&p);
  p.path                    = xstrdup(s_fe.game_path);
  p.force_software_renderer = true;

  if (!system_boot_system(&p, &e)) {
    fprintf(stderr, "system_boot_system failed: %s\n", Error_get_description(&e));
    system_boot_parameters_destroy(&p);
    Error_destroy(&e);
    return false;
  }
  system_boot_parameters_destroy(&p);
  Error_destroy(&e);

  /* Optional low-RAM write watch (CUPID_TRACE_LOW_RAM=1).  Must run AFTER
   * system_boot_system so the bus tap pointer survives any reset during
   * boot.  (Exec-trace init was already done before BIOS boot.) */
  cpu_code_cache_dbg_maybe_install_low_ram_watch();

  /* Top-level run loop.  system_execute() returns whenever execution gets
   * interrupted (SDL_QUIT, ESC, save state, etc.); we keep calling it
   * until the user actually asked to exit. */
  while (!s_fe.quit_requested && !system_is_shutdown()) {
    system_execute();
    if (s_fe.quit_requested)
      break;
    /* If we got here without a quit, the system either paused or stopped;
     * don't busy-loop; present once and pump events to give the user a
     * chance to either close the window or unpause. */
    host_pump_messages_on_core_thread();
    SDL_Delay(16);
  }

  if (s_dump_on_exit)
    dump_debug_ringbuffers();

  if (system_is_valid())
    system_shutdown_system(false);
  system_core_thread_shutdown();
  system_process_shutdown();
  return true;
}

int main(int argc, char** argv)
{
  memset(&s_fe, 0, sizeof(s_fe));
  s_fe.current_save_slot = 1;

  atexit(dump_debug_ringbuffers);

  {
    struct sigaction sa = {0};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
  }

  /* Crash handler: backtrace on SIGSEGV/SIGABRT/SIGFPE/SIGILL/SIGBUS via
   * common/crash_handler.  page_fault_handler (installed by the JIT init)
   * later overwrites SIGSEGV/SIGBUS to catch fastmem faults, but explicitly
   * routes any non-fastmem SEGV to crash_handler_signal_handler (see
   * page_fault_handler.c:96), so this still produces a backtrace on real
   * crashes after JIT comes up.  Cleanup callback is the audio/log ring
   * dumper already wired via atexit. */
  crash_handler_install(&dump_debug_ringbuffers);

  /* Send core/util log messages to stderr so the user sees them. */
  log_set_console_output_params(true, false);
  log_set_log_level(getenv("CUPID_TRACE") ? LOG_LEVEL_DEV : LOG_LEVEL_INFO);

  /* Probe host CPU once so any subsystem (SW rasterizer dispatcher, future
   * SIMD audio paths) can read the cached results lock-free. */
  cpu_features_init();

  if (!parse_args(argc, argv))
    return 1;

  /* Split BIOS path into directory + filename for the core hooks. */
  char* bios_file = NULL;
  split_path(s_fe.bios_path, &s_fe.bios_dir, &bios_file);
  free(bios_file);

  if (!ensure_dir("./memcards")) {
    /* Not fatal, but warn. */
    fprintf(stderr, "warning: could not create ./memcards; memory cards disabled.\n");
  }
  if (!ensure_dir("./savestates")) {
    /* Not fatal, but F11/F12 will silently fail until the dir exists. */
    fprintf(stderr, "warning: could not create ./savestates; F11/F12 will fail.\n");
  }

  apply_frontend_settings();

  if (!sdl_setup())
    goto fail;

  if (!audio_setup())
    goto fail;

  const bool ok = boot_and_run();

  audio_teardown();
  sdl_teardown();

  /* Free our heap. */
  free(s_fe.bios_path);
  free(s_fe.game_path);
  free(s_fe.bios_dir);
  if (s_fe.ini_inited)
    ini_settings_interface_destroy(&s_fe.ini);
  settings_destroy(&g_gpu_settings);
  settings_destroy(&g_settings);

  return ok ? 0 : 2;

fail:
  audio_teardown();
  sdl_teardown();
  free(s_fe.bios_path);
  free(s_fe.game_path);
  free(s_fe.bios_dir);
  if (s_fe.ini_inited)
    ini_settings_interface_destroy(&s_fe.ini);
  settings_destroy(&g_gpu_settings);
  settings_destroy(&g_settings);
  return 2;
}
