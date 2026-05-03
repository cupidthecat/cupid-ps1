/*
 * See system.h for scope cuts.  The bulk of the original system module is
 * OSD / achievements / cheats / netplay / runahead / media-capture plumbing;
 * those are dropped.  We keep boot, reset, run loop, frame timing, save
 * states, memory-card lifecycle, and region detection.
 */

#include "core/system.h"
#include "core/system_private.h"

#include "core/bios.h"
#include "core/bus.h"
#include "core/cdrom.h"
#include "core/controller.h"
#include "core/cpu_code_cache.h"
#include "core/cpu_core.h"
#include "core/cpu_diff_block.h"
#include "core/cpu_pc_trigger.h"
#include "core/dma.h"
#include "core/game_database.h"
#include "core/gpu.h"
#include "core/gpu_hw_texture_cache.h"
#include "core/gpu_sw.h"
#include "core/host.h"
#include "core/interrupt_controller.h"
#include "core/mdec.h"
#include "core/memory_card.h"
#include "core/multitap.h"
#include "core/pad.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/sio.h"
#include "core/spu.h"
#include "core/timers.h"
#include "core/timing_event.h"

#include "util/audio_stream.h"
#include "util/cd_image.h"
#include "util/compress_helpers.h"
#include "util/iso_reader.h"
#include "util/state_wrapper.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/timer.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

LOG_CHANNEL(System);

/* compile-time constants                                              */

#define FALLBACK_EXE_NAME "PSX.EXE"

enum {
  /* limit on how many frames in a row we may skip displaying. */
  MAX_SKIPPED_DUPLICATE_FRAME_COUNT = 2,
  MAX_SKIPPED_TIMEOUT_FRAME_COUNT   = 1,
  MEMORY_CARD_FAST_FORWARD_FRAMES   = 30,
};

/* weak-extern fallbacks for sibling modules whose headers don't yet  */
/* expose every helper we touch.                                       */

 /* GPU clock-changed hook: gpu.h doesn't declare one, but we emit an explicit
 * weak ref so a future port can wire it. */
__attribute__((weak)) extern void gpu_clock_changed(void);
/* PGXP lifecycle.  Names declared in cpu_pgxp.h are `pgxp_*` (no
 * `cpu_` prefix); the prior weak `cpu_pgxp_*` externs silently resolved
 * to NULL function pointers and the runtime-`if (fn)` guards turned every
 * call into a no-op, so PGXP was never actually initialized.  Symptom
 * was a SIGSEGV in pgxp_get_ptr (s_mem still NULL) when --pgxp on
 * triggered gpu_commands.c:352 → pgxp_get_precise_vertex. */
extern void pgxp_initialize(void);
extern void pgxp_shutdown(void);
extern void pgxp_reset(void);
extern void pgxp_do_state(state_wrapper_t* sw);
__attribute__((weak)) extern void cpu_code_cache_reset(void);
__attribute__((weak)) extern void cpu_code_cache_invalidate_all_ram_blocks(void);
__attribute__((weak)) extern void performance_counters_reset(void);
__attribute__((weak)) extern void performance_counters_clear(void);
__attribute__((weak)) extern u32  cpu_pgxp_get_state_size(bool enable_8mb_ram);

/* Frontend-provided host hooks; default no-ops if the SDL2 frontend
 * doesn't yet implement them. */
void host_on_settings_reloaded(void);
void host_on_system_starting(void);
void host_on_system_started(void);
void host_on_system_stopping(void);
void host_on_system_destroyed(void);
void host_on_system_paused(void);
void host_on_system_resumed(void);
void host_pump_messages_on_core_thread(void);
void host_input_manager_poll_sources(void);

__attribute__((weak)) void host_on_settings_reloaded(void) {}
__attribute__((weak)) void host_on_system_starting(void)  {}
__attribute__((weak)) void host_on_system_started(void)   {}
__attribute__((weak)) void host_on_system_stopping(void)  {}
__attribute__((weak)) void host_on_system_destroyed(void) {}
__attribute__((weak)) void host_on_system_paused(void)    {}
__attribute__((weak)) void host_on_system_resumed(void)   {}
__attribute__((weak)) void host_pump_messages_on_core_thread(void) {}
__attribute__((weak)) void host_input_manager_poll_sources(void)   {}

/* state                                                               */

typedef struct {
  tick_count_t ticks_per_second;
  tick_count_t max_slice_ticks;

  u32 frame_number;
  u32 internal_frame_number;

  console_region_t   region;
  system_state_t     state;
  system_boot_mode_t boot_mode;

  bool system_executing;
  bool system_interrupted;
  bool frame_step_request;

  bool throttler_enabled;
  bool optimal_frame_pacing;
  bool skip_presenting_duplicate_frames;

  bool fast_forward_enabled;
  bool turbo_enabled;

  u8   memory_card_fast_forward_frames;

  u32  skipped_frame_count;
  u32  last_presented_internal_frame_number;

  float video_frame_rate;
  float target_speed;

  timer_value_t frame_period;
  timer_value_t next_frame_time;

  timer_value_t frame_start_time;

  const bios_image_info_t* bios_image_info;
  bios_hash_t              bios_hash;
  u32                      taints;

  /* heap-owned; "" if none. */
  char*                    running_game_path;
  char*                    running_game_serial;
  char*                    running_game_title;
  char*                    exe_override;
  u8*                      exe_buffer;       /* heap-owned PS-X EXE; NULL when not BOOT_EXE */
  size_t                   exe_buffer_size;
  game_hash_t              running_game_hash;

  /* GameDB lookup result for the currently running disc.  NULL when no disc
   * is booted, when serial extraction failed, or when the serial isn't in the
   * shipped gamedb.yaml.  Pointer is owned by the GameDB loader (NOT freed
   * here; it's stable for the process lifetime). */
  const game_database_entry_t* running_game_entry;

  atomic_bool              startup_cancelled;

  /* monotonic timer baseline for play-time accounting. */
  timer_value_t            session_start_time;

  /* outstanding async save tasks (we run them inline, but the counter is
   * still bumped so system_flush_save_states() works as a no-op). */
  atomic_uint              outstanding_save_state_tasks;

  /* process start time, set in system_process_startup. */
  timer_value_t            process_start_time;
} system_state_vars_t;

static system_state_vars_t s_state;

/* forward decls                                                       */

static void           system_internal_reset       (void);
static void           system_destroy_system       (void);
static void           system_clear_running_game   (void);
static bool           system_load_bios            (Error* error);
static bool           system_set_boot_mode        (system_boot_mode_t new_boot_mode,
                                                   disc_region_t disc_region, Error* error);
static bool           system_initialize           (cd_image_t* disc, disc_region_t disc_region,
                                                   bool force_software_renderer, Error* error);
static void           system_update_running_game  (const char* path, cd_image_t* image, bool booting);
static void           system_update_controllers   (void);
static void           system_reset_controllers    (void);
static void           system_update_multitaps     (void);
static bool           system_do_state             (state_wrapper_t* sw, bool update_display);
static bool           system_save_state_to_buffer (u8** out_data, size_t* out_size, Error* error);
static bool           system_load_state_data_from_buffer(const u8* data, size_t size, u32 version,
                                                         Error* error, bool update_display);
static bool           system_load_state_buffer_from_file(FILE* fp, u8** out_state_data,
                                                          size_t* out_state_size, u32* out_version,
                                                         char** out_media_path,
                                                         u32* out_media_subimage_index, 
                                                         Error* error);
static bool           system_save_state_buffer_to_file(FILE* fp, const u8* state_data, size_t state_size,
                                                       const char* media_path, u32 media_subimage_index,
                                                       Error* error);
static void           system_throttle              (timer_value_t current_time, timer_value_t sleep_until);
static void           system_update_throttle_period(void);
static void           system_reset_throttler       (void);
static bool           system_is_fast_forwarding_boot(void);
static void           system_check_for_and_exit_execution(void);
static void           system_set_taints_from_settings(void);
static u8             system_get_audio_output_volume(void);

/* dup / strndup helpers that tolerate NULL input. */
static char* xstrdup_or_null(const char* s)
{
  if (!s) return NULL;
  size_t n = strlen(s);
  char* r = (char*)malloc(n + 1);
  if (!r) return NULL;
  memcpy(r, s, n + 1);
  return r;
}

static void xfree_str(char** p)
{
  if (p && *p) { free(*p); *p = NULL; }
}

 /* Sets *p to a new copy of `value` (free-and-replace semantics).  `value`
 * may be NULL or "" to clear. */
static void set_str(char** p, const char* value)
{
  xfree_str(p);
  if (value && value[0])
    *p = xstrdup_or_null(value);
}

static const char* str_or_empty(const char* s) { return s ? s : ""; }

/* ctor / dtor for boot params + extended state info                  */

void system_boot_parameters_init(system_boot_parameters_t* p)
{
  memset(p, 0, sizeof(*p));
}

void system_boot_parameters_destroy(system_boot_parameters_t* p)
{
  if (!p) return;
  xfree_str(&p->path);
  xfree_str(&p->save_state);
  xfree_str(&p->override_exe);
}

void system_save_state_info_destroy(system_save_state_info_t* ssi)
{
  if (!ssi) return;
  xfree_str(&ssi->path);
}

void system_extended_save_state_info_destroy(system_extended_save_state_info_t* essi)
{
  if (!essi) return;
  xfree_str(&essi->title);
  xfree_str(&essi->serial);
  xfree_str(&essi->media_path);
  if (essi->screenshot_pixels) {
    free(essi->screenshot_pixels);
    essi->screenshot_pixels = NULL;
  }
}

/* state queries                                                       */

system_state_t system_get_state(void)         { return s_state.state; }
bool system_is_running(void)                  { return s_state.state == SYSTEM_STATE_RUNNING; }
bool system_is_paused(void)                   { return s_state.state == SYSTEM_STATE_PAUSED; }
bool system_is_shutdown(void)                 { return s_state.state == SYSTEM_STATE_SHUTDOWN; }
bool system_is_valid(void)                    { return s_state.state == SYSTEM_STATE_RUNNING || s_state.state == SYSTEM_STATE_PAUSED; }
bool system_is_valid_or_initializing(void)    { return s_state.state == SYSTEM_STATE_STARTING ||
                                                       s_state.state == SYSTEM_STATE_RUNNING ||
                                                       s_state.state == SYSTEM_STATE_PAUSED; }
bool system_is_executing(void)                { return s_state.system_executing; }

static bool system_is_execution_interrupted(void)
{
  return s_state.state != SYSTEM_STATE_RUNNING || s_state.system_interrupted;
}

static void system_check_for_and_exit_execution(void)
{
  if (system_is_execution_interrupted()) {
    s_state.system_interrupted = false;
    timing_events_cancel_running_event();
    cpu_exit_execution();
  }
}

bool system_is_startup_cancelled(void)
{
  return atomic_load_explicit(&s_state.startup_cancelled, memory_order_acquire);
}

void system_cancel_pending_startup(void)
{
  if (s_state.state == SYSTEM_STATE_STARTING)
    atomic_store_explicit(&s_state.startup_cancelled, true, memory_order_release);
}

void system_interrupt_execution(void)
{
  if (s_state.system_executing)
    s_state.system_interrupted = true;
}

console_region_t system_get_region(void)    { return s_state.region; }
bool             system_is_pal_region(void) { return s_state.region == CONSOLE_REGION_PAL; }

const char* system_get_taint_name(system_taint_t taint)
{
  static const char* const names[SYSTEM_TAINT_MAX_COUNT] = {
    "CPUOverclock",
    "CDROMReadSpeedup",
    "CDROMSeekSpeedup",
    "ForceFrameTimings",
    "RAM8MB",
    "Cheats",
    "Patches",
    "MemoryCardMismatch", 
  };
  if ((u32)taint >= (u32)SYSTEM_TAINT_MAX_COUNT) return "?";
  return names[taint];
}

bool system_has_taint(system_taint_t taint)
{
  return (s_state.taints & (1u << (u8)taint)) != 0u;
}

void system_set_taint(system_taint_t taint)
{
  if (!system_has_taint(taint))
    WARNING_LOG("Setting system taint: %s", system_get_taint_name(taint));
  s_state.taints |= (1u << (u8)taint);
}

tick_count_t system_get_ticks_per_second(void) { return s_state.ticks_per_second; }
tick_count_t system_get_max_slice_ticks (void) { return s_state.max_slice_ticks; }

void system_update_overclock(void)
{
  s_state.ticks_per_second = system_scale_ticks_to_overclock(SYSTEM_MASTER_CLOCK);
  s_state.max_slice_ticks  = system_scale_ticks_to_overclock(SYSTEM_MASTER_CLOCK / 10);
  spu_cpu_clock_changed();
  cdrom_cpu_clock_changed();
  if (gpu_clock_changed) gpu_clock_changed();
  timers_cpu_clocks_changed();
  system_update_throttle_period();
}

global_ticks_t system_get_global_tick_counter(void)
{
  if (timing_events_is_running_events())
    return timing_events_get_event_run_tick_counter();
  return timing_events_get_global_tick_counter() + cpu_get_pending_ticks();
}

u32 system_get_frame_number(void)          { return s_state.frame_number; }
u32 system_get_internal_frame_number(void) { return s_state.internal_frame_number; }

const char* system_get_game_title (void)    { return str_or_empty(s_state.running_game_title); }
const char* system_get_game_serial(void)    { return str_or_empty(s_state.running_game_serial); }
const char* system_get_game_path  (void)    { return str_or_empty(s_state.running_game_path); }
const char* system_get_exe_override(void)   { return str_or_empty(s_state.exe_override); }

void system_handle_kernel_initialized(void)
{
  if (s_state.boot_mode != SYSTEM_BOOT_MODE_BOOT_EXE || !s_state.exe_buffer ||
      s_state.exe_buffer_size == 0u)
    return;

  Error e;
  Error_init(&e);
  if (!bus_inject_executable(s_state.exe_buffer, s_state.exe_buffer_size,
                             /*set_pc=*/true, &e)) {
    ERROR_LOG("EXE injection failed: %s", Error_get_description(&e));
    Error_destroy(&e);
    return;
  }
  Error_destroy(&e);
  INFO_LOG("Injected EXE: %s", str_or_empty(s_state.exe_override));
  /* RAM regions covering the EXE just got rewritten; toss any compiled
   * blocks that may have been built from BIOS-shell-era contents. */
  if (cpu_code_cache_invalidate_all_ram_blocks)
    cpu_code_cache_invalidate_all_ram_blocks();
  /* We are inside a recomp/interp store callback for the BIOS POST=07
   * write.  cpu_set_pc only updates npc; without bailing out of the
   * current block, the BIOS continues executing past the POST write and
   * a later jr/jal clobbers npc, sending the kernel back into shell
   * boot.  longjmp to cpu_execute()'s setjmp; system_execute() loops
   * and re-enters with the EXE entry as pc. */
  cpu_exit_execution();
}

game_hash_t        system_get_game_hash(void)        { return s_state.running_game_hash; }
bool               system_is_running_unknown_game(void){ return s_state.running_game_hash == 0; }
system_boot_mode_t system_get_boot_mode(void)        { return s_state.boot_mode; }

const game_database_entry_t* system_get_game_database_entry(void)
{
  return s_state.running_game_entry;
}

bool system_is_using_known_ps1_bios(void)
{
  return s_state.bios_image_info && s_state.bios_image_info->fastboot_patch == BIOS_FAST_BOOT_PATCH_TYPE1;
}

/* path discrimination                                                 */

static bool ends_with_no_case(const char* path, const char* suffix)
{
  if (!path) return false;
  const u32 path_len = (u32)strlen(path);
  const u32 suf_len  = (u32)strlen(suffix);
  return string_util_ends_with_no_case_view(path, path_len, suffix, suf_len);
}

bool system_is_disc_path(const char* path)
{
  return (ends_with_no_case(path, ".bin") || ends_with_no_case(path, ".cue") ||
          ends_with_no_case(path, ".img") || ends_with_no_case(path, ".iso") ||
          ends_with_no_case(path, ".chd") || ends_with_no_case(path, ".ecm") ||
          ends_with_no_case(path, ".mds") || ends_with_no_case(path, ".pbp") ||
          ends_with_no_case(path, ".ccd") || ends_with_no_case(path, ".m3u"));
}

bool system_is_exe_path(const char* path)
{
  return (ends_with_no_case(path, ".exe") || ends_with_no_case(path, ".psexe") ||
          ends_with_no_case(path, ".ps-exe") || ends_with_no_case(path, ".psx") ||
          ends_with_no_case(path, ".cpe") || ends_with_no_case(path, ".elf"));
}

bool system_is_psf_path(const char* path)
{
  return (ends_with_no_case(path, ".psf") || ends_with_no_case(path, ".minipsf"));
}

bool system_is_gpu_dump_path(const char* path)
{
  return (ends_with_no_case(path, ".psxgpu") || ends_with_no_case(path, ".psxgpu.zst") ||
          ends_with_no_case(path, ".psxgpu.xz"));
}

bool system_is_loadable_path(const char* path)
{
  return system_is_disc_path(path) || system_is_exe_path(path) ||
         system_is_psf_path(path)  || system_is_gpu_dump_path(path);
}

bool system_is_save_state_path(const char* path)
{
  return ends_with_no_case(path, ".sav");
}

console_region_t system_get_console_region_for_disc_region(disc_region_t region)
{
  switch (region) {
    case DISC_REGION_NTSC_J: return CONSOLE_REGION_NTSC_J;
    case DISC_REGION_PAL:    return CONSOLE_REGION_PAL;
    case DISC_REGION_NTSC_U:
    case DISC_REGION_OTHER:
    case DISC_REGION_NON_PS1:
    default:                 return CONSOLE_REGION_NTSC_U;
  }
}

void system_get_game_hash_id(game_hash_t hash, char* out, size_t out_size)
{
  snprintf(out, out_size, "HASH-%llX", (unsigned long long)hash);
}

/* region detection from disc                                           */

disc_region_t system_get_region_for_serial(const char* serial)
{
  if (!serial || !serial[0]) return DISC_REGION_OTHER;

  static const struct { const char* prefix; disc_region_t region; } prefixes[] = {
    {"sces", DISC_REGION_PAL},     {"sced", DISC_REGION_PAL},
    {"sles", DISC_REGION_PAL},     {"sled", DISC_REGION_PAL},
    {"scps", DISC_REGION_NTSC_J},  {"slps", DISC_REGION_NTSC_J},
    {"slpm", DISC_REGION_NTSC_J},  {"sczs", DISC_REGION_NTSC_J},
    {"papx", DISC_REGION_NTSC_J},
    {"scus", DISC_REGION_NTSC_U},  {"slus", DISC_REGION_NTSC_U},
  };
  const u32 serial_len = (u32)strlen(serial);
  for (size_t i = 0; i < sizeof(prefixes)/sizeof(prefixes[0]); i++) {
    const u32 plen = (u32)strlen(prefixes[i].prefix);
    if (string_util_starts_with_no_case_view(serial, serial_len, prefixes[i].prefix, plen))
      return prefixes[i].region;
  }
  return DISC_REGION_OTHER;
}

disc_region_t system_get_region_from_system_area(cd_image_t* cdi)
{
  if (!cdi) return DISC_REGION_OTHER;

  /* License is on sector 4 of track 1. */
  if (cd_image_get_track_mode(cdi, 1) == CD_IMAGE_TRACK_MODE_AUDIO)
    return DISC_REGION_OTHER;
  if (!cd_image_seek_track_lba(cdi, 1, 4))
    return DISC_REGION_OTHER;

  u8 raw[CD_IMAGE_RAW_SECTOR_SIZE];
  if (!cd_image_read_raw_sector(cdi, raw, NULL))
    return DISC_REGION_OTHER;

  const u8* sector_data = NULL;
  u32       data_len = 0;
  if (!iso_reader_extract_sector_data(raw, ISO_READER_READ_MODE_DATA,
                                      &sector_data, &data_len, NULL))
    return DISC_REGION_OTHER;

  static const char ntsc_u_string[] = "          Licensed  by          Sony Computer Entertainment Amer  ica ";
  static const char ntsc_j_string[] = "          Licensed  by          Sony Computer Entertainment Inc.";
  static const char pal_string[]    = "          Licensed  by          Sony Computer Entertainment Euro pe";

  if (data_len >= sizeof(ntsc_u_string) - 1 &&
      memcmp(sector_data, ntsc_u_string, sizeof(ntsc_u_string) - 1) == 0)
    return DISC_REGION_NTSC_U;
  if (data_len >= sizeof(ntsc_j_string) - 1 &&
      memcmp(sector_data, ntsc_j_string, sizeof(ntsc_j_string) - 1) == 0)
    return DISC_REGION_NTSC_J;
  if (data_len >= sizeof(pal_string) - 1 &&
      memcmp(sector_data, pal_string, sizeof(pal_string) - 1) == 0)
    return DISC_REGION_PAL;
  return DISC_REGION_OTHER;
}

/* Reads SYSTEM.CNF from the ISO and returns the boot executable path
 * (heap-allocated) or NULL. */
static char* system_get_executable_name_for_image(iso_reader_t* iso, bool strip_subdirectories)
{
  u8*    cnf_data = NULL;
  size_t cnf_size = 0;
  if (!iso_reader_read_file_path(iso, "SYSTEM.CNF", &cnf_data, &cnf_size,
                                 ISO_READER_READ_MODE_DATA, NULL))
    return xstrdup_or_null(FALLBACK_EXE_NAME);

  /* Find "BOOT = ..." line. */
  char* result = NULL;
  size_t i = 0;
  while (i < cnf_size && !result) {
    /* skip whitespace at line start */
    while (i < cnf_size && (cnf_data[i] == ' ' || cnf_data[i] == '\t' ||
                             cnf_data[i] == '\r' || cnf_data[i] == '\n'))
      i++;

    /* read key */
    size_t k_start = i;
    while (i < cnf_size && cnf_data[i] != '=' && cnf_data[i] != '\n' && cnf_data[i] != '\r')
      i++;
    size_t k_end = i;
    while (k_end > k_start && (cnf_data[k_end-1] == ' ' || cnf_data[k_end-1] == '\t'))
      k_end--;

    if (i < cnf_size && cnf_data[i] == '=') {
      /* check key */
      const bool is_boot = ((k_end - k_start) == 4 &&
                            (cnf_data[k_start]   == 'B' || cnf_data[k_start]   == 'b') &&
                            (cnf_data[k_start+1] == 'O' || cnf_data[k_start+1] == 'o') &&
                            (cnf_data[k_start+2] == 'O' || cnf_data[k_start+2] == 'o') && 
                            (cnf_data[k_start+3] == 'T' || cnf_data[k_start+3] == 't'));
      i++; /* skip '=' */
      while (i < cnf_size && (cnf_data[i] == ' ' || cnf_data[i] == '\t'))
        i++;
      size_t v_start = i;
      while (i < cnf_size && cnf_data[i] != '\n' && cnf_data[i] != '\r')
        i++;
      size_t v_end = i;
      while (v_end > v_start && (cnf_data[v_end-1] == ' ' || cnf_data[v_end-1] == '\t'))
        v_end--;

      if (is_boot && v_end > v_start) {
        const size_t len = v_end - v_start;
        result = (char*)malloc(len + 1);
        if (!result) break;
        memcpy(result, cnf_data + v_start, len);
        result[len] = '\0';
      }
    }

    /* advance past line break */
    while (i < cnf_size && (cnf_data[i] == '\r' || cnf_data[i] == '\n'))
      i++;
  }
  free(cnf_data);

  if (!result)
    return xstrdup_or_null(FALLBACK_EXE_NAME);

  /* Process result: */
  if (strip_subdirectories) {
    char* slash = strrchr(result, '\\');
    if (!slash) slash = strchr(result, ':');
    if (slash) {
      const size_t tail_len = strlen(slash + 1) + 1;
      memmove(result, slash + 1, tail_len);
    }
  } else {
    if (strncmp(result, "cdrom:", 6) == 0) {
      memmove(result, result + 6, strlen(result + 6) + 1);
    }
    while (result[0] == '/' || result[0] == '\\')
      memmove(result, result + 1, strlen(result + 1) + 1);
  }
  /* strip ;version */
  char* semi = strchr(result, ';');
  if (semi) *semi = '\0';

  return result;
}

static bool system_read_executable_from_image_inner(iso_reader_t* iso,
                                                    char** out_executable_name,
                                                    u8**   out_executable_data,
                                                    size_t* out_executable_size)
{
  char* exe_path = system_get_executable_name_for_image(iso, false);
  if (!exe_path) return false;

  if (out_executable_data && exe_path[0]) {
    if (!iso_reader_read_file_path(iso, exe_path, out_executable_data, out_executable_size,
                                   ISO_READER_READ_MODE_DATA, NULL)) {
      ERROR_LOG("Failed to read executable '%s' from disc", exe_path);
      free(exe_path);
      return false;
    }
  }

  if (out_executable_name) {
    *out_executable_name = exe_path;
  } else {
    free(exe_path);
  }
  return true;
}

/* xxhash is not yet ported.  We approximate the gamehash with a stable
 * FNV-1a over (exe_name, exe_data, pvd, track_1_length).  cupid-ps1 doesn't
 * ship a database, so the value just needs to be stable per (image, exe)
 * tuple. */
static game_hash_t system_compute_game_hash(const char* exe_name, const u8* exe_data, size_t exe_size,
                                            const void* pvd, size_t pvd_size, u32 track_1_length)
{
  u64 h = 0xCBF29CE484222325ull;
  for (size_t i = 0; exe_name && exe_name[i]; i++) {
    h ^= (u8)exe_name[i];
    h *= 0x100000001B3ull;
  }
  for (size_t i = 0; i < exe_size; i++) {
    h ^= exe_data[i];
    h *= 0x100000001B3ull;
  }
  const u8* pvd_bytes = (const u8*)pvd;
  for (size_t i = 0; i < pvd_size; i++) {
    h ^= pvd_bytes[i];
    h *= 0x100000001B3ull;
  }
  for (u32 m = track_1_length, i = 0; i < 4; i++, m >>= 8) {
    h ^= (u8)m;
    h *= 0x100000001B3ull;
  }
  return (game_hash_t)h;
}

bool system_get_game_details_from_image(cd_image_t* cdi, char** out_id_or_null,
                                         game_hash_t* out_hash_or_null,
                                        char** out_executable_name_or_null, 
                                        u8** out_executable_data_or_null,
                                        size_t* out_executable_size_or_null)
{
  iso_reader_t iso;
  iso_reader_init(&iso);
  char*  exe_name  = NULL;
  u8*    exe_data  = NULL;
  size_t exe_size  = 0;

  if (!iso_reader_open(&iso, cdi, 1, NULL) ||
      !system_read_executable_from_image_inner(&iso, &exe_name, &exe_data, &exe_size)) {
    iso_reader_destroy(&iso);
    if (out_id_or_null)            *out_id_or_null = NULL;
    if (out_hash_or_null)          *out_hash_or_null = 0;
    if (out_executable_name_or_null) *out_executable_name_or_null = NULL;
    if (out_executable_data_or_null) *out_executable_data_or_null = NULL;
    if (out_executable_size_or_null) *out_executable_size_or_null = 0;
    return false;
  }

  const u32 track_len = (u32)cd_image_get_track_length(cdi, 1);
  const game_hash_t hash =
    system_compute_game_hash(exe_name, exe_data, exe_size,
                             iso_reader_get_pvd(&iso), sizeof(iso_primary_volume_descriptor_t),
                             track_len);
  DEV_LOG("Hash for '%s' - %016llX", exe_name, (unsigned long long)hash);

  /* Build serial id from exe_name. */
  if (out_id_or_null) {
    if (strcmp(exe_name, FALLBACK_EXE_NAME) == 0) {
      char buf[64];
      system_get_game_hash_id(hash, buf, sizeof(buf));
      *out_id_or_null = xstrdup_or_null(buf);
    } else {
      const char* slash = strrchr(exe_name, '\\');
      const char* base  = slash ? (slash + 1) : exe_name;
      const size_t blen = strlen(base);
      char* id = (char*)malloc(blen + 1);
      if (id) {
        size_t out_pos = 0;
        for (size_t i = 0; i < blen; i++) {
          const char c = base[i];
          if (c == '.') continue;
          id[out_pos++] = (c == '_') ? '-' : (char)toupper((unsigned char)c);
        }
        id[out_pos] = '\0';
        *out_id_or_null = id;
      } else {
        *out_id_or_null = NULL;
      }
    }
  }

  if (out_hash_or_null)            *out_hash_or_null = hash;
  if (out_executable_name_or_null) *out_executable_name_or_null = exe_name;
  else                             free(exe_name);
  if (out_executable_data_or_null) *out_executable_data_or_null = exe_data;
  else if (exe_data)               free(exe_data);
  if (out_executable_size_or_null) *out_executable_size_or_null = exe_size;

  iso_reader_destroy(&iso);
  return true;
}

game_hash_t system_get_game_hash_from_file(const char* path)
{
  u8*    data = NULL;
  size_t size = 0;
  if (!fs_read_binary_file_path(path, &data, &size, NULL))
    return 0;
  const char* file_name = NULL;
  u32         file_name_len = 0;
  path_get_file_name_cstr(path, &file_name, &file_name_len);
  char buf[260];
  if (file_name_len >= sizeof(buf)) file_name_len = (u32)sizeof(buf) - 1u;
  memcpy(buf, file_name, file_name_len);
  buf[file_name_len] = '\0';
  const game_hash_t h =
    system_compute_game_hash(buf, data, size, NULL, 0, 0);
  free(data);
  return h;
}

disc_region_t system_get_region_for_image(cd_image_t* cdi)
{
  const disc_region_t sa = system_get_region_from_system_area(cdi);
  if (sa != DISC_REGION_OTHER) return sa;

  iso_reader_t iso;
  iso_reader_init(&iso);
  if (!iso_reader_open(&iso, cdi, 1, NULL)) {
    iso_reader_destroy(&iso);
    return DISC_REGION_NON_PS1;
  }

  char* exename = system_get_executable_name_for_image(&iso, false);
  if (!exename || !exename[0] || !iso_reader_file_exists(&iso, exename, NULL)) {
    free(exename);
    iso_reader_destroy(&iso);
    return DISC_REGION_NON_PS1;
  }

  const char* slash = strrchr(exename, '\\');
  const disc_region_t r = system_get_region_for_serial(slash ? slash + 1 : exename);
  free(exename);
  iso_reader_destroy(&iso);
  return r;
}

disc_region_t system_get_region_for_exe(const char* path)
{
  FILE* fp = fs_open_file(path, "rb", NULL);
  if (!fp) return DISC_REGION_OTHER;

  bios_psexe_header_t header;
  const size_t got = fread(&header, 1, sizeof(header), fp);
  fclose(fp);
  if (got != sizeof(header)) return DISC_REGION_OTHER;
  return bios_get_psexe_disc_region(&header);
}

/* settings                                                            */

void system_load_settings(bool display_osd_messages)
{
  (void)display_osd_messages;
  /* g_settings is reset and reloaded by the frontend through settings_load();
   * here we just propagate to subsystems that need it. */
  if (system_is_valid())
    system_update_speed_limiter_state();
}

void system_apply_settings(bool display_osd_messages)
{
  (void)display_osd_messages;
  /* See comment on system_load_settings: in cupid-ps1 the frontend re-runs
   * settings_load() and then calls us so subsystems can re-pick values. */
  if (system_is_valid()) {
    system_update_overclock();
    system_update_throttle_period();
    system_update_speed_limiter_state();
    system_update_running_game(system_get_game_path(), NULL, false);
  }
  host_on_settings_reloaded();
}

void system_reload_input_sources       (void) {}
void system_reload_input_bindings      (void) {}
void system_update_controller_settings (void)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    controller_t* c = pad_get_controller(i);
    if (!c) continue;
    char section[16];
    settings_get_controller_settings_section(i, section, sizeof(section));
    /* settings_interface_t* may be NULL in headless mode; skip. */
    (void)section;
    (void)c;
  }
}

/* boot / shutdown                                                     */

bool system_boot_system(system_boot_parameters_t* parameters, Error* error)
{
  if (!parameters) {
    Error_set_string(error, "boot parameters NULL");
    return false;
  }

  timer_t_ boot_timer;
  timer_init(&boot_timer);

  if (parameters->path && parameters->path[0])
    INFO_LOG("Boot Path: %s", parameters->path);
  else
    INFO_LOG("Boot Path: <BIOS/Shell>");

  cd_image_t*           disc  = NULL;
  console_region_t      auto_console_region = CONSOLE_REGION_NTSC_U;
  disc_region_t         disc_region = DISC_REGION_NON_PS1;
  system_boot_mode_t    boot_mode = SYSTEM_BOOT_MODE_FULL_BOOT;
  char*                 exe_override = NULL;

  if (parameters->path && parameters->path[0]) {
    if (system_is_exe_path(parameters->path)) {
      boot_mode = SYSTEM_BOOT_MODE_BOOT_EXE;
      exe_override = xstrdup_or_null(parameters->path);
    } else if (system_is_psf_path(parameters->path)) {
      boot_mode = SYSTEM_BOOT_MODE_BOOT_PSF;
      exe_override = xstrdup_or_null(parameters->path);
    } else if (system_is_gpu_dump_path(parameters->path)) {
      Error_set_string(error, "GPU dump replay is not supported in cupid-ps1.");
      return false;
    }

    if (boot_mode == SYSTEM_BOOT_MODE_BOOT_EXE) {
      const disc_region_t exe_region = system_get_region_for_exe(parameters->path);
      INFO_LOG("EXE region detected as %d", (int)exe_region);
      auto_console_region = system_get_console_region_for_disc_region(exe_region);
    } else if (boot_mode == SYSTEM_BOOT_MODE_BOOT_PSF) {
      auto_console_region = CONSOLE_REGION_NTSC_U; /* PSF region detection not ported */
    } else {
      INFO_LOG("Loading CD image '%s'...", parameters->path);
      disc = cd_image_open(parameters->path, error);
      if (!disc) {
        Error_add_prefix_fmt(error, "Failed to open CD image '%s':\n", parameters->path);
        free(exe_override);
        return false;
      }
      if (parameters->media_playlist_index != 0 &&
          !cd_image_switch_sub_image(disc, parameters->media_playlist_index, error)) {
        Error_add_prefix_fmt(error, "Failed to switch to subimage %u in '%s':\n",
                             parameters->media_playlist_index, parameters->path);
        cd_image_destroy(disc);
        free(exe_override);
        return false;
      }
      disc_region = system_get_region_for_image(disc);
      auto_console_region = system_get_console_region_for_disc_region(disc_region);
      INFO_LOG("Auto-detected console region for '%s' (disc region %d)",
               parameters->path, (int)disc_region);
    }
  }

  /* commit to starting state */
  if (s_state.state != SYSTEM_STATE_SHUTDOWN) {
    Error_set_string(error, "system already running");
    cd_image_destroy(disc);
    free(exe_override);
    return false;
  }
  s_state.state  = SYSTEM_STATE_STARTING;
  s_state.region = auto_console_region;
  atomic_store_explicit(&s_state.startup_cancelled, false, memory_order_relaxed);
  host_on_system_starting();

  system_update_running_game(disc ? cd_image_get_path(disc) : str_or_empty(parameters->path),
                             disc, true);

  /* Per-game compatibility overlay: layered on top of g_settings AFTER user
   * settings have been read by the frontend.  Must run before fast-boot is
   * decided (so ForceFullBoot wins) and before system_initialize / controller
   * setup so the controller-type fixups land before pad_initialize. */
  if (g_settings.apply_compatibility_settings && s_state.running_game_entry) {
    game_database_entry_apply_settings(s_state.running_game_entry, &g_settings,
                                       /*display_osd_messages=*/true);
  }

  /* user override of region. */
  s_state.region =
    (g_settings.region == CONSOLE_REGION_AUTO) ? auto_console_region : g_settings.region;
  INFO_LOG("Console region: %d", (int)s_state.region);

  /* exe override */
  if (parameters->override_exe && parameters->override_exe[0]) {
    if (!fs_file_exists(parameters->override_exe) ||
        !system_is_exe_path(parameters->override_exe)) {
      Error_set_string_fmt(error, "File '%s' is not a valid executable to boot.",
                            parameters->override_exe);
      host_on_system_stopping();
      system_destroy_system();
      free(exe_override);
      return false;
    }
    INFO_LOG("Overriding boot executable: '%s'", parameters->override_exe);
    boot_mode = SYSTEM_BOOT_MODE_BOOT_EXE;
    free(exe_override);
    exe_override = xstrdup_or_null(parameters->override_exe);
  }

  /* fast boot? */
  if (boot_mode == SYSTEM_BOOT_MODE_FULL_BOOT && disc_region != DISC_REGION_NON_PS1) {
    bool want_fast = g_settings.bios_patch_fast_boot;
    if (parameters->override_fast_boot.has) want_fast = parameters->override_fast_boot.val;
    if (want_fast) boot_mode = SYSTEM_BOOT_MODE_FAST_BOOT;
  }

  if (!system_set_boot_mode(boot_mode, disc_region, error) ||
      !system_initialize(disc, disc_region, parameters->force_software_renderer, error)) {
    host_on_system_stopping();
    system_destroy_system();
    free(exe_override);
    return false;
  }

  set_str(&s_state.exe_override, exe_override);
  free(exe_override);

  /* Load PS-X EXE into a held buffer once at boot.  Injection happens later
   * from kernel_initialized_hook() (after BIOS POST=07).  Buffer survives
   * system_reset_system() so a reset re-injects on the next kernel-init. */
  if (s_state.boot_mode == SYSTEM_BOOT_MODE_BOOT_EXE) {
    free(s_state.exe_buffer);
    s_state.exe_buffer = NULL;
    s_state.exe_buffer_size = 0;
    if (!fs_read_binary_file_path(s_state.exe_override, &s_state.exe_buffer,
                                  &s_state.exe_buffer_size, error)) {
      Error_add_prefix_fmt(error, "Failed to read executable '%s':\n",
                           s_state.exe_override);
      host_on_system_stopping();
      system_destroy_system();
      return false;
    }
    bios_psexe_header_t hdr;
    if (s_state.exe_buffer_size < sizeof(hdr) ||
        (memcpy(&hdr, s_state.exe_buffer, sizeof(hdr)),
         !bios_is_valid_psexe_header(&hdr, s_state.exe_buffer_size))) {
      Error_set_string_fmt(error, "File '%s' is not a valid PS-X EXE.",
                           s_state.exe_override);
      host_on_system_stopping();
      system_destroy_system();
      return false;
    }
  }

  system_update_controllers();
  system_update_memory_cards();
  system_update_multitaps();
  system_internal_reset();

  s_state.state = SYSTEM_STATE_RUNNING;

  /* immediately pause? */
  const bool start_paused = parameters->override_start_paused.has &&
                             parameters->override_start_paused.val;

  /* try to load save state */
  if (parameters->save_state && parameters->save_state[0]) {
    const int rc = system_load_state(parameters->save_state, error, false, start_paused);
    if (rc != 1) {
      Error_add_prefix_fmt(error, "Failed to load save state file '%s':\n", parameters->save_state);
      host_on_system_stopping();
      system_destroy_system();
      return false;
    }
  }

  host_on_system_started();

  if (parameters->load_image_to_ram || g_settings.cdrom_load_image_to_ram)
    cdrom_precache_media();

  if (start_paused) system_pause_system(true);

  system_update_speed_limiter_state();

  INFO_LOG("System booted in %.2f ms", timer_get_milliseconds(&boot_timer));
  if (performance_counters_reset) performance_counters_reset();
  system_reset_throttler();
  return true;
}

static bool system_initialize(cd_image_t* disc, disc_region_t disc_region,
                              bool force_software_renderer, Error* error)
{
  s_state.ticks_per_second = system_scale_ticks_to_overclock(SYSTEM_MASTER_CLOCK);
  s_state.max_slice_ticks  = system_scale_ticks_to_overclock(SYSTEM_MASTER_CLOCK / 10);
  s_state.frame_number          = 1;
  s_state.internal_frame_number = 0;

  s_state.target_speed = g_settings.emulation_speed;
  s_state.video_frame_rate = 60.0f;
  s_state.frame_period = 0;
  s_state.next_frame_time = 0;
  s_state.turbo_enabled = false;
  s_state.fast_forward_enabled = false;

  timing_events_initialize();

  bus_initialize();
  cpu_initialize();
  if (!cpu_code_cache_process_startup(error)) {
    /* JIT bring-up failed (typically: host can't allocate the 48 MB JIT
     * region).  If the user picked recompiler explicitly we honour that and
     * bail; if it was the default we silently demote to interpreter so the
     * run still succeeds.  cpu_code_cache_execute already falls through to
     * the interpreter when no backend has been registered. */
    if (g_settings.cpu_execution_mode == CPU_EXECUTION_MODE_RECOMPILER) {
      WARNING_LOG("Recompiler init failed; falling back to interpreter.");
      g_settings.cpu_execution_mode = CPU_EXECUTION_MODE_INTERPRETER;
      Error_clear(error);
    } else {
      return false;
    }
  }
  cpu_diff_block_install();
  cpu_pc_trigger_install();
  cdrom_initialize();

  if (disc && !cdrom_insert_media(disc, disc_region,
                                   str_or_empty(s_state.running_game_serial),
                                   strlen(str_or_empty(s_state.running_game_serial)),
                                   str_or_empty(s_state.running_game_title),
                                   strlen(str_or_empty(s_state.running_game_title)),
                                   "", 0, error)) {
    return false;
  }

  /* SW is the only renderer; instantiate the backend before gpu_initialize
   * so it can publish via g_gpu_backend during register/state setup. */
  if (!g_gpu_backend && !gpu_sw_create()) {
    Error_set_string(error, "Failed to create software GPU backend");
    return false;
  }
  gpu_initialize();
  (void)force_software_renderer; /* SW is the only path; no renderer choice */

  if (g_settings.gpu_pgxp_enable) pgxp_initialize();

  dma_initialize();
  pad_initialize();
  timers_initialize();
  spu_initialize();
  mdec_initialize();
  sio_initialize();

  system_update_throttle_period();

  if (performance_counters_clear) performance_counters_clear();
  return true;
}

static void system_destroy_system(void)
{
  if (s_state.state == SYSTEM_STATE_SHUTDOWN) return;

  sio_shutdown();
  mdec_shutdown();
  spu_shutdown();
  timers_shutdown();
  pad_shutdown();
  cdrom_shutdown();
  gpu_shutdown();
  dma_shutdown();
  pgxp_shutdown();
  cpu_code_cache_process_shutdown();
  cpu_shutdown();
  bus_shutdown();
  timing_events_shutdown();

  system_clear_running_game();

  s_state.taints = 0;
  memset(s_state.bios_hash, 0, sizeof(s_state.bios_hash));
  s_state.bios_image_info = NULL;
  xfree_str(&s_state.exe_override);
  free(s_state.exe_buffer);
  s_state.exe_buffer = NULL;
  s_state.exe_buffer_size = 0;
  s_state.boot_mode = SYSTEM_BOOT_MODE_NONE;
  s_state.state = SYSTEM_STATE_SHUTDOWN;

  host_on_system_destroyed();
}

void system_abnormal_shutdown(const char* reason)
{
  if (!system_is_valid()) return;
  ERROR_LOG("Abnormal shutdown: %s", reason ? reason : "(null)");
  s_state.state = SYSTEM_STATE_STOPPING;
  host_on_system_stopping();
  if (s_state.system_executing)
    system_interrupt_execution();
  else
    system_destroy_system();
}

static void system_clear_running_game(void)
{
  xfree_str(&s_state.running_game_serial);
  xfree_str(&s_state.running_game_path);
  xfree_str(&s_state.running_game_title);
  s_state.running_game_hash  = 0;
  /* GameDB entry is loader-owned; just drop the borrowed pointer. */
  s_state.running_game_entry = NULL;
}

void system_shutdown_system(bool save_resume_state)
{
  if (!system_is_valid()) return;

  if (save_resume_state) {
    Error e;
    Error_init(&e);
    if (!system_save_resume_state(&e)) {
      WARNING_LOG("Failed to save resume state: %s", Error_get_description(&e));
    }
    Error_destroy(&e);
  }

  host_on_system_stopping();
  s_state.state = SYSTEM_STATE_STOPPING;
  if (!s_state.system_executing)
    system_destroy_system();
}

/* reset                                                               */

static void system_internal_reset(void)
{
  if (system_is_shutdown()) return;

  system_set_taints_from_settings();

  timing_events_reset();
  cpu_reset();
  if (cpu_code_cache_reset) cpu_code_cache_reset();
  if (g_settings.gpu_pgxp_enable) pgxp_reset();

  bus_reset();
  dma_reset();
  interrupt_controller_reset();
  gpu_reset(true);
  cdrom_reset();
  pad_reset();
  timers_reset();
  spu_reset();
  mdec_reset();
  sio_reset();

  s_state.frame_number = 1;
  s_state.internal_frame_number = 0;
}

void system_reset_system(void)
{
  if (!system_is_valid()) return;

  /* Boot-mode reset: preserve BootEXE/BootPSF, otherwise re-evaluate fast vs. full. */
  const system_boot_mode_t new_boot_mode =
    (s_state.boot_mode == SYSTEM_BOOT_MODE_BOOT_EXE ||
     s_state.boot_mode == SYSTEM_BOOT_MODE_BOOT_PSF)
      ? s_state.boot_mode 
      : (g_settings.bios_patch_fast_boot ? SYSTEM_BOOT_MODE_FAST_BOOT : SYSTEM_BOOT_MODE_FULL_BOOT);
  Error e;
  Error_init(&e);
  if (!system_set_boot_mode(new_boot_mode, cdrom_get_disc_region(), &e))
    ERROR_LOG("Failed to reload BIOS on boot mode change: %s", Error_get_description(&e));
  Error_destroy(&e);

  system_internal_reset();

  if (system_is_fast_forwarding_boot())
    system_update_speed_limiter_state();

  if (performance_counters_reset) performance_counters_reset();
  system_reset_throttler();
  system_interrupt_execution();
}

void system_pause_system(bool paused)
{
  if (paused == system_is_paused() || !system_is_valid())
    return;

  s_state.state = paused ? SYSTEM_STATE_PAUSED : SYSTEM_STATE_RUNNING;

  if (paused) {
    host_on_system_paused();
  } else {
    host_on_system_resumed();
    if (performance_counters_reset) performance_counters_reset();
    system_reset_throttler();
  }
}

/* BIOS load + boot mode                                               */

static bool system_load_bios(Error* error)
{
  bios_image_t image = {0};
  if (!bios_get_image(s_state.region, &image, error))
    return false;

  s_state.bios_image_info = image.info;
  memcpy(s_state.bios_hash, image.hash, sizeof(bios_hash_t));
  if (image.info)
    INFO_LOG("Using BIOS: %s", image.info->description);
  else {
    tiny_string_t hs;
    tiny_string_init(&hs);
    bios_image_info_get_hash_string(&hs, image.hash);
    WARNING_LOG("Using an unknown BIOS: %s", small_string_c_str(&hs.s));
    small_string_destroy(&hs.s);
  }

  if (image.data && image.data_size >= BUS_BIOS_SIZE)
    memcpy(bus_get_bios_pointer(), image.data, BUS_BIOS_SIZE);

  bios_image_destroy(&image);
  return true;
}

static bool system_set_boot_mode(system_boot_mode_t new_boot_mode,
                                 disc_region_t disc_region, Error* error)
{
  const bool can_fast_boot =
    (disc_region != DISC_REGION_NON_PS1) &&
    (s_state.state == SYSTEM_STATE_STARTING || 
     (s_state.bios_image_info && bios_supports_fast_boot(s_state.bios_image_info)));

  system_boot_mode_t actual = new_boot_mode;
  if (new_boot_mode == SYSTEM_BOOT_MODE_FAST_BOOT ||
      (new_boot_mode == SYSTEM_BOOT_MODE_FULL_BOOT && s_state.bios_image_info &&
       !bios_can_slow_boot_disc(s_state.bios_image_info, disc_region))) {
    actual = can_fast_boot ? SYSTEM_BOOT_MODE_FAST_BOOT : SYSTEM_BOOT_MODE_FULL_BOOT;
  }

  if (actual == s_state.boot_mode)
    return true;

  if (new_boot_mode != SYSTEM_BOOT_MODE_REPLAY_GPU_DUMP && !system_load_bios(error))
    return false;

  s_state.boot_mode =
    (actual == SYSTEM_BOOT_MODE_FULL_BOOT && s_state.bios_image_info &&
     !bios_can_slow_boot_disc(s_state.bios_image_info, disc_region)) ? 
      SYSTEM_BOOT_MODE_FAST_BOOT : actual;

  if (s_state.boot_mode == SYSTEM_BOOT_MODE_FAST_BOOT) {
    if (s_state.bios_image_info && bios_supports_fast_boot(s_state.bios_image_info)) {
      INFO_LOG("Patching BIOS for fast boot.");
      if (!bios_patch_fast_boot(bus_get_bios_pointer(), BUS_BIOS_SIZE,
                                s_state.bios_image_info->fastboot_patch))
        s_state.boot_mode = SYSTEM_BOOT_MODE_FULL_BOOT;
    } else {
      ERROR_LOG("Cannot fast boot, BIOS is incompatible.");
      s_state.boot_mode = SYSTEM_BOOT_MODE_FULL_BOOT;
    }
  }

  /* If full-boot without a disc on auto region, switch to BIOS region. */
  if (new_boot_mode == SYSTEM_BOOT_MODE_FULL_BOOT && disc_region == DISC_REGION_NON_PS1 &&
      s_state.bios_image_info && g_settings.region == CONSOLE_REGION_AUTO &&
      s_state.region != s_state.bios_image_info->region) {
    WARNING_LOG("Changing region to match BIOS for full boot without game.");
    s_state.region = s_state.bios_image_info->region;
  }

  return true;
}

/* save state                                                          */

size_t system_get_max_save_state_size(bool enable_8mb_ram)
{
  return enable_8mb_ram ? (11u * 1024u * 1024u) : (5u * 1024u * 1024u);
}

static bool system_do_state(state_wrapper_t* sw, bool update_display)
{
  (void)update_display;
  if (!state_wrapper_do_marker(sw, "System")) return false;

  if (state_wrapper_get_version(sw) < 74u) {
    u32 r = (u32)s_state.region;
    state_wrapper_do_u32(sw, &r);
    s_state.region = (console_region_t)r;
  } else {
    u8 r = (u8)s_state.region;
    state_wrapper_do_u8(sw, &r);
    s_state.region = (console_region_t)r;
  }

  if (state_wrapper_get_version(sw) >= 75u) {
    u32 t = s_state.taints;
    state_wrapper_do_u32(sw, &t);
    if (state_wrapper_is_reading(sw))
      s_state.taints |= t;
  }

  state_wrapper_do_u32(sw, &s_state.frame_number);
  state_wrapper_do_u32(sw, &s_state.internal_frame_number);

  if (state_wrapper_get_version(sw) >= 58u) {
    bios_hash_t hash;
    memcpy(hash, s_state.bios_hash, sizeof(hash));
    state_wrapper_do_bytes(sw, hash, sizeof(hash));
    /* No OSD warning; log only. */
    if (state_wrapper_is_reading(sw) &&
        memcmp(hash, s_state.bios_hash, sizeof(hash)) != 0)
      WARNING_LOG("Save state BIOS hash differs from current; running anyway.");
  }

  if (!state_wrapper_do_marker(sw, "CPU") || !cpu_do_state(sw)) return false;
  pgxp_do_state(sw);

  if (state_wrapper_is_reading(sw)) {
    if (cpu_code_cache_reset) cpu_code_cache_reset();
  }

  if (!state_wrapper_do_marker(sw, "Bus")  || !bus_do_state(sw))  return false;
  if (!state_wrapper_do_marker(sw, "DMA")  || !dma_do_state(sw))  return false;
  if (!state_wrapper_do_marker(sw, "InterruptController") ||
      !interrupt_controller_do_state(sw)) return false;
  if (!state_wrapper_do_marker(sw, "GPU")  || !gpu_do_state(sw))  return false;
  if (!state_wrapper_do_marker(sw, "CDROM")|| !cdrom_do_state(sw))return false;
  if (!state_wrapper_do_marker(sw, "Pad")  || !pad_do_state(sw, false)) return false;
  if (!state_wrapper_do_marker(sw, "Timers")||!timers_do_state(sw))return false;
  if (!state_wrapper_do_marker(sw, "SPU")  || !spu_do_state(sw))  return false;
  if (!state_wrapper_do_marker(sw, "MDEC") || !mdec_do_state(sw)) return false;
  if (!state_wrapper_do_marker(sw, "SIO")  || !sio_do_state(sw))  return false;

  if (!state_wrapper_do_marker(sw, "Events") || !timing_events_do_state(sw))
    return false;

  if (!state_wrapper_do_marker(sw, "Overclock")) return false;

  bool oc_active = g_settings.cpu_overclock_active;
  u32  oc_num    = g_settings.cpu_overclock_numerator;
  u32  oc_den    = g_settings.cpu_overclock_denominator;
  state_wrapper_do_bool(sw, &oc_active);
  state_wrapper_do_u32 (sw, &oc_num);
  state_wrapper_do_u32 (sw, &oc_den);

  if (state_wrapper_is_reading(sw) &&
      (oc_active != g_settings.cpu_overclock_active ||
       (oc_active && (g_settings.cpu_overclock_numerator   != oc_num || 
                      g_settings.cpu_overclock_denominator != oc_den)))) {
    WARNING_LOG("CPU overclock differs from save state; updating.");
    system_update_overclock();
  }

  /* Skip Cheevos marker; achievements are dropped, but we honour the
   * marker to keep version compat with the binary save-state format. */
  if (state_wrapper_get_version(sw) >= 56u) {
    if (!state_wrapper_do_marker(sw, "Cheevos")) return false;
    /* Achievements::DoState was a length-prefixed byte stream; we simply
     * preserve the marker but write 0 bytes / read & skip 0 bytes. */
  }

  if (state_wrapper_has_error(sw)) return false;
  return true;
}

static bool system_load_state_data_from_buffer(const u8* data, size_t size, u32 version,
                                               Error* error, bool update_display)
{
  if (system_is_shutdown()) {
    Error_set_string(error, "system invalid");
    return false;
  }
  state_wrapper_t sw;
  state_wrapper_init_read(&sw, data, size, version);
  if (!system_do_state(&sw, update_display)) {
    Error_set_string(error, "save-state stream is corrupted.");
    return false;
  }
  system_interrupt_execution();
  if (performance_counters_reset) performance_counters_reset();
  system_reset_throttler();
  return true;
}

static bool system_save_state_to_buffer(u8** out_data, size_t* out_size, Error* error)
{
  if (system_is_shutdown()) {
    Error_set_string(error, "system invalid");
    return false;
  }

  /* worst-case allocation; trim later */
  const size_t cap = system_get_max_save_state_size(bus_get_ram_size() > BUS_RAM_2MB_SIZE);
  u8* buf = (u8*)malloc(cap);
  if (!buf) {
    Error_set_string(error, "failed to allocate save-state buffer");
    return false;
  }

  state_wrapper_t sw;
  state_wrapper_init_write(&sw, buf, cap, SAVE_STATE_VERSION);
  if (!system_do_state(&sw, false)) {
    Error_set_string(error, "DoState failed");
    free(buf);
    return false;
  }
  *out_data = buf;
  *out_size = state_wrapper_get_position(&sw);
  return true;
}

static bool fread_exact(void* dst, size_t n, FILE* fp, Error* error)
{
  if (n == 0) return true;
  if (fread(dst, 1, n, fp) != n) {
    Error_set_errno(error, errno);
    return false;
  }
  return true;
}

static bool system_load_state_buffer_from_file(FILE* fp, u8** out_state_data,
                                                size_t* out_state_size, u32* out_version,
                                               char** out_media_path,
                                               u32* out_media_subimage_index,
                                               Error* error) 
{
  *out_state_data = NULL;
  *out_state_size = 0;
  if (out_media_path) *out_media_path = NULL;
  if (out_media_subimage_index) *out_media_subimage_index = 0;

  save_state_header_t header;
  if (!fread_exact(&header, sizeof(header), fp, error))
    return false;
  if (header.magic != SAVE_STATE_MAGIC) {
    Error_set_string(error, "save state magic mismatch");
    return false;
  }
  if (header.version < SAVE_STATE_MINIMUM_VERSION) {
    Error_set_string_fmt(error, "save state version %u < minimum %u",
                          header.version, (unsigned)SAVE_STATE_MINIMUM_VERSION);
    return false;
  }
  if (header.version > SAVE_STATE_VERSION) {
    Error_set_string_fmt(error, "save state version %u > current %u",
                          header.version, (unsigned)SAVE_STATE_VERSION);
    return false;
  }
  *out_version = header.version;

  /* media path */
  if (out_media_path && header.media_path_length > 0) {
    if (!fs_fseek64_e(fp, (s64)header.offset_to_media_path, SEEK_SET, error)) return false;
    char* mp = (char*)malloc((size_t)header.media_path_length + 1u);
    if (!mp) {
      Error_set_string(error, "alloc media-path");
      return false;
    }
    if (!fread_exact(mp, header.media_path_length, fp, error)) {
      free(mp);
      return false;
    }
    mp[header.media_path_length] = '\0';
    *out_media_path = mp;
    if (out_media_subimage_index) *out_media_subimage_index = header.media_subimage_index;
  }

  /* Compressed save states: dispatch through compress_helpers.  Deflate
   * and Zstandard are supported; XZ remains deferred. */
  if (header.data_uncompressed_size > SAVE_STATE_MAX_SAVE_STATE_SIZE) {
    Error_set_string(error, "save-state body too large");
    return false;
  }

  if (!fs_fseek64_e(fp, (s64)header.offset_to_data, SEEK_SET, error)) return false;

  compress_type_t compress_type;
  switch (header.data_compression_type) {
    case SAVE_STATE_COMPRESSION_TYPE_NONE:
      compress_type = COMPRESS_TYPE_UNCOMPRESSED;
      break;
    case SAVE_STATE_COMPRESSION_TYPE_DEFLATE:
      compress_type = COMPRESS_TYPE_DEFLATE;
      break;
    case SAVE_STATE_COMPRESSION_TYPE_ZSTANDARD:
      compress_type = COMPRESS_TYPE_ZSTANDARD;
      break;
    case SAVE_STATE_COMPRESSION_TYPE_XZ:
    default:
      Error_set_string_fmt(error, "compressed save states (type %u) are not supported in cupid-ps1",
                            header.data_compression_type);
      return false;
  }

  u8* buf;
  if (compress_type == COMPRESS_TYPE_UNCOMPRESSED) {
    buf = (u8*)malloc(header.data_uncompressed_size);
    if (!buf) {
      Error_set_string(error, "alloc state body");
      return false;
    }
    if (!fread_exact(buf, header.data_uncompressed_size, fp, error)) {
      free(buf);
      return false;
    }
  } else {
    /* Read compressed body, decompress to fixed-size buffer. */
    u8* cbuf = (u8*)malloc(header.data_compressed_size);
    if (!cbuf) {
      Error_set_string(error, "alloc state compressed body");
      return false;
    }
    if (!fread_exact(cbuf, header.data_compressed_size, fp, error)) {
      free(cbuf);
      return false;
    }
    buf = (u8*)malloc(header.data_uncompressed_size);
    if (!buf) {
      free(cbuf);
      Error_set_string(error, "alloc state body");
      return false;
    }
    if (!compress_helpers_decompress_buffer(compress_type, buf, header.data_uncompressed_size,
                                            cbuf, header.data_compressed_size, error)) {
      free(cbuf);
      free(buf);
      return false;
    }
    free(cbuf);
  }
  *out_state_data = buf;
  *out_state_size = header.data_uncompressed_size;
  return true;
}

static bool system_save_state_buffer_to_file(FILE* fp, const u8* state_data, size_t state_size,
                                             const char* media_path, u32 media_subimage_index,
                                             Error* error)
{
  save_state_header_t header;
  memset(&header, 0, sizeof(header));
  header.magic   = SAVE_STATE_MAGIC;
  header.version = SAVE_STATE_VERSION;
  string_util_strlcpy_cstr(header.title, str_or_empty(s_state.running_game_title), sizeof(header.title));
  string_util_strlcpy_cstr(header.serial,str_or_empty(s_state.running_game_serial), sizeof(header.serial));

  /* placeholder write; we fix up offsets and rewrite. */
  if (fwrite(&header, sizeof(header), 1, fp) != 1) {
    Error_set_errno(error, errno);
    return false;
  }
  u32 file_pos = (u32)sizeof(header);

  if (media_path && media_path[0]) {
    header.offset_to_media_path = file_pos;
    header.media_path_length    = (u32)strlen(media_path);
    header.media_subimage_index = media_subimage_index;
    if (fwrite(media_path, header.media_path_length, 1, fp) != 1) {
      Error_set_errno(error, errno);
      return false;
    }
    file_pos += header.media_path_length;
  }

  /* skip screenshot; not generated by software-only path */

  header.offset_to_data         = file_pos;
  header.data_compression_type  = SAVE_STATE_COMPRESSION_TYPE_NONE;
  header.data_compressed_size   = (u32)state_size;
  header.data_uncompressed_size = (u32)state_size;
  if (fwrite(state_data, state_size, 1, fp) != 1) {
    Error_set_errno(error, errno);
    return false;
  }

  if (!fs_fseek64_e(fp, 0, SEEK_SET, error)) return false;
  if (fwrite(&header, sizeof(header), 1, fp) != 1 || fflush(fp) != 0) {
    Error_set_errno(error, errno);
    return false;
  }
  return true;
}

int system_load_state(const char* path, Error* error, bool save_undo_state, bool force_update_display)
{
  (void)save_undo_state;
  if (!system_is_valid()) {
    Error_set_string(error, "system not in correct state");
    return 0;
  }

  system_flush_save_states();

  /* .sav.gz / .sav.zst / .sav.xz: outer-wrap the entire save-state file.
   * Read whole file → decompress to memory → fmemopen so the existing
   * header parser works unchanged. */
  const compress_type_t outer = compress_helpers_type_from_path(path);
  FILE* fp;
  u8*   wrapped_buf = NULL;
  if (outer == COMPRESS_TYPE_UNCOMPRESSED) {
    fp = fs_open_file(path, "rb", error);
    if (!fp) {
      Error_add_prefix_fmt(error, "Failed to open '%s': ", path);
      return 0;
    }
  } else {
    u8*    fbuf  = NULL;
    size_t fsize = 0;
    if (!fs_read_binary_file_path(path, &fbuf, &fsize, error)) {
      Error_add_prefix_fmt(error, "Failed to read '%s': ", path);
      return 0;
    }
    if (!compress_helpers_decompress_alloc(outer, fbuf, fsize, 0,
                                           &wrapped_buf, &fsize, error)) {
      free(fbuf);
      Error_add_prefix_fmt(error, "Failed to decompress '%s': ", path);
      return 0;
    }
    free(fbuf);
    fp = fmemopen(wrapped_buf, fsize, "rb");
    if (!fp) {
      free(wrapped_buf);
      Error_set_errno(error, errno);
      return 0;
    }
  }

  INFO_LOG("Loading state from '%s'...", path);

  u8*    state_data = NULL;
  size_t state_size = 0;
  u32    version    = 0;
  char*  media_path = NULL;
  u32    media_subimage_index = 0;
  const bool ok = system_load_state_buffer_from_file(fp, &state_data, &state_size, &version,
                                                     &media_path, &media_subimage_index, error);
  fclose(fp);
  free(wrapped_buf);
  if (!ok) {
    free(state_data);
    free(media_path);
    return 0;
  }

  /* If the save state references a different disc, or the same disc on a
   * different sub-image, swap. */
  if (media_path && media_path[0]) {
    const char* curr     = cdrom_get_media_path();
    const u32   curr_sub = cdrom_has_media() ? cdrom_get_current_sub_image() : 0u;
    const bool  same     = curr && strcmp(curr, media_path) == 0 && curr_sub == media_subimage_index;
    if (!same) {
      cd_image_t* new_disc = cd_image_open(media_path, error);
      bool swap_ok = (new_disc != NULL);
      if (swap_ok && media_subimage_index != 0 && cd_image_has_sub_images(new_disc)) {
        swap_ok = cd_image_switch_sub_image(new_disc, media_subimage_index, error);
      }
      const disc_region_t r = swap_ok ? system_get_region_for_image(new_disc) : DISC_REGION_NON_PS1;
      if (swap_ok) {
        system_update_running_game(media_path, new_disc, false);
        swap_ok = cdrom_insert_media(new_disc, r,
                                     str_or_empty(s_state.running_game_serial),
                                     strlen(str_or_empty(s_state.running_game_serial)),
                                     str_or_empty(s_state.running_game_title),
                                     strlen(str_or_empty(s_state.running_game_title)),
                                     "", 0, error);
      }
      if (!swap_ok) {
        cd_image_destroy(new_disc);
        WARNING_LOG("Failed to swap disc to '%s' (sub-image %u) for save state; keeping current.",
                    media_path, media_subimage_index);
      }
    }
  }
  free(media_path);

  const bool ld_ok = system_load_state_data_from_buffer(state_data, state_size, version,
                                                        error, force_update_display);
  free(state_data);
  return ld_ok ? 1 : 0;
}

bool system_save_state(const char* path, Error* error, bool backup_existing_save,
                       bool ignore_memcard_busy)
{
  if (!system_is_valid()) {
    Error_set_string(error, "system not in correct state");
    return false;
  }
  if (!ignore_memcard_busy && system_is_saving_memory_cards()) {
    Error_set_string(error, "memory card is being saved");
    return false;
  }

  u8* state_data = NULL;
  size_t state_size = 0;
  if (!system_save_state_to_buffer(&state_data, &state_size, error))
    return false;

  if (backup_existing_save && fs_file_exists(path)) {
    small_string_stack_t bak;
    small_string_stack_init(&bak);
    path_change_extension_cstr(&bak.s, path, "bak");
    fs_rename_path(path, small_string_c_str(&bak.s), NULL);
    small_string_destroy(&bak.s);
  }

  /* .sav.gz / .sav.zst / .sav.xz: serialize to a tmpfile (the writer seeks
   * back to rewrite the header. glibc open_memstream truncates on
   * back-seek so we need a real seekable file), read it back into memory,
   * then compress as the outer wrapper. */
  const compress_type_t outer = compress_helpers_type_from_path(path);
  FILE* fp;
  if (outer == COMPRESS_TYPE_UNCOMPRESSED) {
    fp = fs_open_file(path, "wb", error);
    if (!fp) {
      free(state_data);
      return false;
    }
  } else {
    fp = tmpfile();
    if (!fp) {
      free(state_data);
      Error_set_errno(error, errno);
      return false;
    }
  }

  const char* media_path = cdrom_has_media() ? cdrom_get_media_path() : "";
  const u32 sub_idx      = cdrom_has_media() ? cdrom_get_current_sub_image() : 0u;
  bool ok = system_save_state_buffer_to_file(fp, state_data, state_size,
                                             media_path, sub_idx, error);
  free(state_data);

  if (ok && outer != COMPRESS_TYPE_UNCOMPRESSED) {
    u8*    raw = NULL;
    size_t raw_len = 0;
    if (fs_fseek64_e(fp, 0, SEEK_SET, error) &&
        fs_read_binary_file(fp, &raw, &raw_len, error)) {
      u8*    cbuf = NULL;
      size_t clen = 0;
      ok = compress_helpers_compress_alloc(outer, raw, raw_len, -1, &cbuf, &clen, error);
      free(raw);
      if (ok) {
        FILE* out = fs_open_file(path, "wb", error);
        if (!out) {
          free(cbuf);
          fclose(fp);
          return false;
        }
        ok = (fwrite(cbuf, 1, clen, out) == clen);
        if (!ok) Error_set_errno(error, errno);
        fclose(out);
        free(cbuf);
      }
    } else {
      ok = false;
    }
  }
  fclose(fp);
  return ok;
}

bool system_save_resume_state(Error* error)
{
  if (!s_state.running_game_serial || !s_state.running_game_serial[0]) {
    Error_set_string(error, "Cannot save resume state without serial.");
    return false;
  }
  small_string_stack_t path;
  small_string_stack_init(&path);
  system_get_game_save_state_path(s_state.running_game_serial, -1, &path.s);
  const bool ok = system_save_state(small_string_c_str(&path.s), error, false, true);
  small_string_destroy(&path.s);
  return ok;
}

void system_get_game_save_state_path(const char* serial, s32 slot, small_string_t* out)
{
  small_string_clear(out);
  if (slot < 0)
    small_string_sprintf(out, "savestates/%s_resume.sav", str_or_empty(serial));
  else
    small_string_sprintf(out, "savestates/%s_%d.sav", str_or_empty(serial), (int)slot);
}

void system_get_global_save_state_path(s32 slot, small_string_t* out)
{
  small_string_clear(out);
  if (slot < 0)
    small_string_assign_cstr(out, "savestates/resume.sav");
  else
    small_string_sprintf(out, "savestates/savestate_%d.sav", (int)slot);
}

void system_get_most_recent_resume_save_state_path(small_string_t* out)
{
  small_string_clear(out);
}

void system_load_state_from_slot(bool global, s32 slot)
{
  if (!system_is_valid()) return;
  small_string_stack_t path;
  small_string_stack_init(&path);
  if (global) system_get_global_save_state_path(slot, &path.s);
  else        system_get_game_save_state_path(s_state.running_game_serial, slot, &path.s);
  if (!fs_file_exists(small_string_c_str(&path.s))) {
    WARNING_LOG("No save state in slot %d.", (int)slot);
  } else {
    Error e; Error_init(&e);
    const int r = system_load_state(small_string_c_str(&path.s), &e, true, false);
    if (r != 1)
      WARNING_LOG("Failed to load slot %d: %s", (int)slot, Error_get_description(&e));
    Error_destroy(&e);
  }
  small_string_destroy(&path.s);
}

void system_save_state_to_slot(bool global, s32 slot)
{
  if (!system_is_valid()) return;
  small_string_stack_t path;
  small_string_stack_init(&path);
  if (global) system_get_global_save_state_path(slot, &path.s);
  else        system_get_game_save_state_path(s_state.running_game_serial, slot, &path.s);
  Error e; Error_init(&e);
  if (!system_save_state(small_string_c_str(&path.s), &e,
                         g_settings.create_save_state_backups, false))
    WARNING_LOG("Failed to save slot %d: %s", (int)slot, Error_get_description(&e));
  Error_destroy(&e);
  small_string_destroy(&path.s);
}

void system_flush_save_states(void)
{
  while (atomic_load_explicit(&s_state.outstanding_save_state_tasks, memory_order_acquire) > 0)
    host_wait_for_all_async_tasks();
}

/* run loop                                                            */

void system_execute(void)
{
  for (;;) {
    switch (s_state.state) {
      case SYSTEM_STATE_RUNNING:
        s_state.system_executing = true;
        timing_events_commit_leftover_ticks();
        cpu_execute();
        s_state.system_executing = false;
        continue;

      case SYSTEM_STATE_STOPPING:
        system_destroy_system();
        return;

      case SYSTEM_STATE_PAUSED:
      case SYSTEM_STATE_SHUTDOWN:
      case SYSTEM_STATE_STARTING:
      default:
        return;
    }
  }
}

void system_single_step_cpu(void)
{
  /* cpu_set_single_step_flag isn't exposed; rely on regular execute exit. */
  if (system_is_paused())
    system_pause_system(false);
}

void system_increment_frame_number(void)
{
  s_state.frame_number++;
}

void system_increment_internal_frame_number(void)
{
  if (system_is_fast_forwarding_boot()) {
    s_state.internal_frame_number++;
    system_update_speed_limiter_state();
    return;
  }
  s_state.internal_frame_number++;
}

void system_frame_done(void)
{
  /* End-of-frame plumbing.  Achievements / cheats / dumper / runahead all
   * drop here; we keep MDEC end-of-frame and SPU drain. */
  mdec_end_frame();
  spu_generate_pending_samples();

  timer_value_t current_time = timer_get_current_value();

  /* memory card fast-forward */
  if (s_state.memory_card_fast_forward_frames > 0) {
    s_state.memory_card_fast_forward_frames--;
    if (s_state.memory_card_fast_forward_frames == 0)
      system_update_speed_limiter_state();
  }

  /* frame step */
  if (s_state.frame_step_request) {
    s_state.frame_step_request = false;
    system_pause_system(true);
  }

  /* throttle */
  if (s_state.throttler_enabled)
    system_throttle(current_time, s_state.next_frame_time);

  s_state.frame_start_time = current_time;

  host_pump_messages_on_core_thread();
  host_input_manager_poll_sources();

  system_check_for_and_exit_execution();
}

float system_get_video_frame_rate(void) { return s_state.video_frame_rate; }

void system_set_video_frame_rate(float frequency)
{
  if (s_state.video_frame_rate == frequency) return;
  VERBOSE_LOG("Video frame rate: %.2f -> %.2f fps", s_state.video_frame_rate, frequency);
  s_state.video_frame_rate = frequency;
  system_update_throttle_period();
}

static void system_update_throttle_period(void)
{
  if (s_state.target_speed > 1e-9f && s_state.video_frame_rate > 0.0f) {
    const double ts = (double)s_state.target_speed;
    s_state.frame_period =
      timer_seconds_to_value(1.0 / ((double)s_state.video_frame_rate * ts));
  } else {
    s_state.frame_period = 1;
  }
  system_reset_throttler();
}

static void system_reset_throttler(void)
{
  s_state.next_frame_time = timer_get_current_value() + s_state.frame_period;
}

static void system_throttle(timer_value_t current_time, timer_value_t sleep_until)
{
  if (current_time > sleep_until) {
    /* running too slow; skip ahead. */
    const u64 diff = current_time - s_state.next_frame_time;
    if (s_state.frame_period > 0)
      s_state.next_frame_time += (diff / s_state.frame_period) * s_state.frame_period + s_state.frame_period;
    return;
  }
  timer_sleep_until(sleep_until, false);
  s_state.next_frame_time += s_state.frame_period;
}

float system_get_target_speed       (void) { return s_state.target_speed; }
float system_get_audio_nominal_rate (void) { return s_state.throttler_enabled ? s_state.target_speed : 1.0f; }

bool system_is_running_at_non_standard_speed(void)
{
  return system_is_valid() && s_state.target_speed != 1.0f;
}

void system_update_speed_limiter_state(void)
{
  s_state.target_speed =
    (system_is_fast_forwarding_boot() || s_state.memory_card_fast_forward_frames > 0) ? 0.0f
    : (s_state.turbo_enabled ? g_settings.turbo_speed
       : (s_state.fast_forward_enabled ? g_settings.fast_forward_speed 
                                       : g_settings.emulation_speed));
  s_state.throttler_enabled = (s_state.target_speed != 0.0f);
  s_state.optimal_frame_pacing = (s_state.throttler_enabled && g_settings.display_optimal_frame_pacing);
  s_state.skip_presenting_duplicate_frames =
    s_state.throttler_enabled && g_settings.display_skip_presenting_duplicate_frames;

  VERBOSE_LOG("Target speed: %.0f%%", s_state.target_speed * 100.0f);

  system_update_throttle_period();
  system_reset_throttler();
}

bool system_is_fast_forward_enabled(void) { return s_state.fast_forward_enabled; }

void system_set_fast_forward_enabled(bool enabled)
{
  if (!system_is_valid()) return;
  s_state.fast_forward_enabled = enabled;
  system_update_speed_limiter_state();
}

bool system_is_turbo_enabled(void) { return s_state.turbo_enabled; }

void system_set_turbo_enabled(bool enabled)
{
  if (!system_is_valid()) return;
  s_state.turbo_enabled = enabled;
  system_update_speed_limiter_state();
}

void system_do_frame_step(void)
{
  if (!system_is_valid()) return;
  s_state.frame_step_request = true;
  system_pause_system(false);
}

static bool system_is_fast_forwarding_boot(void)
{
  return g_settings.bios_fast_forward_boot && s_state.internal_frame_number == 0 &&
         s_state.boot_mode == SYSTEM_BOOT_MODE_FAST_BOOT;
}

static u8 system_get_audio_output_volume(void)
{
  return settings_get_audio_output_volume(&g_settings, system_is_running_at_non_standard_speed());
}

/* controllers / memory cards                                          */

controller_t* system_get_controller(u32 slot)
{
  return pad_get_controller(slot);
}

static void system_update_controllers(void)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    pad_set_controller(i, NULL);
    const controller_type_t type = g_settings.controller_types[i];
    if (type != CONTROLLER_TYPE_NONE) {
      controller_t* c = controller_create(type, i);
      if (c) pad_set_controller(i, c);
    }
  }
}

static void system_reset_controllers(void)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    controller_t* c = pad_get_controller(i);
    if (c) controller_reset(c);
  }
}

static void system_update_multitaps(void)
{
  switch (g_settings.multitap_mode) {
    case MULTITAP_MODE_DISABLED:
      multitap_set_enable(pad_get_multitap(0), false, 0);
      multitap_set_enable(pad_get_multitap(1), false, 1);
      break;
    case MULTITAP_MODE_PORT1_ONLY:
      multitap_set_enable(pad_get_multitap(0), true, 0);
      multitap_set_enable(pad_get_multitap(1), false, 1);
      break;
    case MULTITAP_MODE_PORT2_ONLY:
      multitap_set_enable(pad_get_multitap(0), false, 0);
      multitap_set_enable(pad_get_multitap(1), true, 1);
      break;
    case MULTITAP_MODE_BOTH_PORTS:
      multitap_set_enable(pad_get_multitap(0), true, 0);
      multitap_set_enable(pad_get_multitap(1), true, 1);
      break;
    default: break;
  }
}

void system_update_memory_cards(void)
{
  if (s_state.running_game_path && system_is_psf_path(s_state.running_game_path))
    return;

  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    const memory_card_type_t type = g_settings.memory_card_types[i];
    if (type == MEMORY_CARD_TYPE_NONE) {
      pad_set_memory_card(i, NULL);
      continue;
    }
    /* Per-game / per-title etc. derivation lives in settings.c; we just open
     * the configured path here. */
    const char* path = g_settings.memory_card_paths[i];
    memory_card_t* card = (path && path[0]) ? memory_card_open(i, path) : memory_card_create(i);
    pad_set_memory_card(i, card);
    if (path) INFO_LOG("Memory card %u: %s", i + 1, path);
  }
}

bool system_has_memory_card(u32 slot)
{
  return pad_get_memory_card(slot) != NULL;
}

bool system_is_saving_memory_cards(void)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    memory_card_t* mc = pad_get_memory_card(i);
    if (mc && memory_card_is_or_was_recently_writing(mc))
      return true;
  }
  return false;
}

void system_swap_memory_cards(void)
{
  if (!system_is_valid()) return;
  memory_card_t* a = pad_remove_memory_card(0);
  memory_card_t* b = pad_remove_memory_card(1);
  pad_set_memory_card(0, b);
  pad_set_memory_card(1, a);
}

void system_on_memory_card_accessed(void)
{
  if (!g_settings.memory_card_fast_forward_access) return;
  const u8 prev = s_state.memory_card_fast_forward_frames;
  s_state.memory_card_fast_forward_frames = MEMORY_CARD_FAST_FORWARD_FRAMES;
  if (prev == 0) system_update_speed_limiter_state();
}

/* media                                                               */

bool system_dump_ram(const char* path, Error* error)
{
  if (!system_is_valid()) {
    Error_set_string(error, "invalid system state");
    return false;
  }
  return fs_write_binary_file(path, bus_get_unprotected_ram_pointer(), bus_get_ram_size(), error);
}

bool system_dump_spu_ram(const char* path, Error* error)
{
  if (!system_is_valid()) {
    Error_set_string(error, "invalid system state");
    return false;
  }
  return fs_write_binary_file(path, spu_get_ram(), SPU_RAM_SIZE, error);
}

bool system_insert_media(const char* path)
{
  Error err; Error_init(&err);
  cd_image_t* image = cd_image_open(path, &err);
  if (!image) {
    WARNING_LOG("Failed to open '%s': %s", path, Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }
  const disc_region_t r = system_get_region_for_image(image);
  system_update_running_game(path, image, false);
  if (!cdrom_insert_media(image, r,
                          str_or_empty(s_state.running_game_serial),
                          strlen(str_or_empty(s_state.running_game_serial)),
                          str_or_empty(s_state.running_game_title),
                          strlen(str_or_empty(s_state.running_game_title)),
                          "", 0, &err)) {
    WARNING_LOG("Insert media failed: %s", Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }
  Error_destroy(&err);
  if (g_settings.cdrom_load_image_to_ram) cdrom_precache_media();
  return true;
}

void system_remove_media(void)
{
  cd_image_t* old = cdrom_remove_media(false);
  cd_image_destroy(old);
}

bool system_has_media_sub_images(void)
{
  const cd_image_t* m = cdrom_get_media();
  return m && cd_image_has_sub_images(m);
}

u32 system_get_media_sub_image_count(void)
{
  const cd_image_t* m = cdrom_get_media();
  return m ? cd_image_get_sub_image_count(m) : 0u;
}

u32 system_get_media_sub_image_index(void)
{
  const cd_image_t* m = cdrom_get_media();
  return m ? cd_image_get_current_sub_image(m) : 0u;
}

char* system_get_media_sub_image_title(u32 index)
{
  const cd_image_t* m = cdrom_get_media();
  return m ? cd_image_get_sub_image_title(m, index) : NULL;
}

bool system_switch_media_sub_image(u32 index, Error* error)
{
  if (!cdrom_has_media()) {
    Error_set_string(error, "No media inserted.");
    return false;
  }

  /* Eject the current image with the disc-swap flag so the cdrom layer
   * extends the spin-down delay (matches Metal Gear Solid expectations). */
  cd_image_t* image = cdrom_remove_media(true);
  if (!image) {
    Error_set_string(error, "No media to swap.");
    return false;
  }

  Error local; Error_init(&local);
  Error* e = error ? error : &local;
  bool ok = cd_image_switch_sub_image(image, index, e);

  disc_region_t r = DISC_REGION_NON_PS1;
  if (ok) {
    r = system_get_region_for_image(image);
    system_update_running_game(cd_image_get_path(image), image, false);
    ok = cdrom_insert_media(image, r,
                            str_or_empty(s_state.running_game_serial),
                            strlen(str_or_empty(s_state.running_game_serial)),
                            str_or_empty(s_state.running_game_title),
                            strlen(str_or_empty(s_state.running_game_title)),
                            "", 0, e);
  }

  if (!ok) {
    WARNING_LOG("Failed to switch to sub-image %u in '%s': %s",
                index, cd_image_get_path(image), Error_get_description(e));
    /* Restore the previous image without changing the sub-image. */
    const disc_region_t rr = system_get_region_for_image(image);
    system_update_running_game(cd_image_get_path(image), image, false);
    if (!cdrom_insert_media(image, rr,
                            str_or_empty(s_state.running_game_serial),
                            strlen(str_or_empty(s_state.running_game_serial)),
                            str_or_empty(s_state.running_game_title),
                            strlen(str_or_empty(s_state.running_game_title)),
                            "", 0, NULL)) {
      cd_image_destroy(image);
    }
    Error_destroy(&local);
    return false;
  }

  Error_destroy(&local);
  return true;
}

/* running-game tracking                                               */

static void system_update_running_game(const char* path, cd_image_t* image, bool booting)
{
  (void)booting;
  set_str(&s_state.running_game_path, path);
  if (xfree_str(&s_state.running_game_serial), xfree_str(&s_state.running_game_title), 0) {}
  s_state.running_game_hash  = 0;
  s_state.running_game_entry = NULL;

  if (path && path[0]) {
    if (system_is_exe_path(path)) {
      const char* fn = NULL;  u32 fnl = 0;
      path_get_file_name_cstr(path, &fn, &fnl);
      if (fnl > 0) {
        char buf[260];
        if (fnl >= sizeof(buf)) fnl = (u32)sizeof(buf) - 1u;
        memcpy(buf, fn, fnl);
        buf[fnl] = '\0';
        set_str(&s_state.running_game_title, buf);
      }
      s_state.running_game_hash = system_get_game_hash_from_file(path);
      if (s_state.running_game_hash != 0) {
        char hid[64];
        system_get_game_hash_id(s_state.running_game_hash, hid, sizeof(hid));
        set_str(&s_state.running_game_serial, hid);
        gpu_texture_cache_game_serial_changed();
      }
    } else if (image && cd_image_get_track_mode(image, 1) != CD_IMAGE_TRACK_MODE_AUDIO) {
      char* id = NULL;
      game_hash_t h = 0;
      system_get_game_details_from_image(image, &id, &h, NULL, NULL, NULL);
      s_state.running_game_hash = h;
      set_str(&s_state.running_game_serial, id);
      free(id);
      gpu_texture_cache_game_serial_changed();

      /* GameDB lookup: use the serial we extracted from SYSTEM.CNF (or, on
       * miss, fall back to whatever cd_image-based heuristic the GameDB
       * helper provides).  The returned entry pointer is loader-owned; we
       * just stash it. */
      game_database_ensure_loaded();
      if (s_state.running_game_serial && s_state.running_game_serial[0]) {
        s_state.running_game_entry =
          game_database_get_entry_for_serial(s_state.running_game_serial);
      }
      if (!s_state.running_game_entry)
        s_state.running_game_entry = game_database_get_entry_for_disc(image);

      /* Prefer the GameDB title when we have it; otherwise fall back to the
       * disc filename. */
      if (s_state.running_game_entry && s_state.running_game_entry->title_len > 0) {
        const game_database_entry_t* e = s_state.running_game_entry;
        char buf[260];
        u32 n = e->title_len;
        if (n >= sizeof(buf)) n = (u32)sizeof(buf) - 1u;
        memcpy(buf, e->title, n);
        buf[n] = '\0';
        set_str(&s_state.running_game_title, buf);
        INFO_LOG("Game serial: %s, title: %s",
                 str_or_empty(s_state.running_game_serial), buf);
      } else {
        const char* fn = NULL;  u32 fnl = 0;
        path_get_file_name_cstr(path, &fn, &fnl);
        char buf[260];
        if (fnl >= sizeof(buf)) fnl = (u32)sizeof(buf) - 1u;
        memcpy(buf, fn, fnl);
        buf[fnl] = '\0';
        set_str(&s_state.running_game_title, buf);
        INFO_LOG("Game serial: %s (no GameDB entry)",
                 str_or_empty(s_state.running_game_serial));
      }
    }
  }
}

/* taints                                                              */

static void system_set_taints_from_settings(void)
{
  s_state.taints = 0;
  if (g_settings.cdrom_read_speedup > 1)
    system_set_taint(SYSTEM_TAINT_CDROM_READ_SPEEDUP);
  if (g_settings.cdrom_seek_speedup > 1)
    system_set_taint(SYSTEM_TAINT_CDROM_SEEK_SPEEDUP);
  if (g_settings.cpu_overclock_active)
    system_set_taint(SYSTEM_TAINT_CPU_OVERCLOCK);
  if (g_settings.gpu_force_video_timing != FORCE_VIDEO_TIMING_MODE_DISABLED)
    system_set_taint(SYSTEM_TAINT_FORCE_FRAME_TIMINGS);
  if (g_settings.cpu_enable_8mb_ram)
    system_set_taint(SYSTEM_TAINT_RAM_8MB);
}

/* process-wide / core-thread lifecycle                                */

bool system_process_startup(Error* error)
{
  if (!bus_allocate_memory(false, error))
    return false;
  s_state.process_start_time = timer_get_current_value();
  return true;
}

void system_process_shutdown(void)
{
  bus_release_memory();
}

bool system_core_thread_initialize(Error* error)
{
  (void)error;
  /* Frontend-side settings load delegated. */
  return true;
}

void system_core_thread_shutdown(void)
{
}

void system_idle_poll_update(void)
{
  /* No achievements / discord / sockets to poll. */
}
