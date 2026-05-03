/*
 * Central glue module: boot/shutdown, main run loop, save/load state, frame
 * timing.  C++ namespace System -> system_ prefix, namespace Host requests
 * become host_request_*.
 *
 * Per the cupid-ps1 scope cuts, ImGui/fullscreenui/OSD/Achievements/Cheats/
 * GameDatabase/Netplay/Runahead/Rewind/MediaCapture/GPU dump/Discord are
 * dropped or stubbed; only save states, memory cards, frame timing, and
 * the SW renderer are kept.  HW renderer selection is preserved as an enum
 * but defaults to GPU_RENDERER_SOFTWARE.
 */

#ifndef CUPID_CORE_SYSTEM_H
#define CUPID_CORE_SYSTEM_H

#include "core/types.h"
#include "core/settings.h"

#include "common/types.h"

#include <stddef.h>
#include <time.h>

typedef struct cd_image           cd_image_t;
typedef struct controller         controller_t;
typedef struct Error              Error;
typedef struct small_string       small_string_t;
typedef struct state_wrapper      state_wrapper_t;

/* Forward decl: full type lives in core/game_database.h.  Kept opaque here so
 * system.h doesn't need to drag in the GameDB headers. */
typedef struct game_database_entry game_database_entry_t;

enum {
  SYSTEM_PER_GAME_SAVE_STATE_SLOTS = 10,
  SYSTEM_GLOBAL_SAVE_STATE_SLOTS   = 10,
  /* 33.8688 MHz CPU clock = 44100 * 0x300. */
  SYSTEM_MASTER_CLOCK              = 44100 * 0x300,
};

typedef enum : u8 {
  SYSTEM_STATE_SHUTDOWN = 0,
  SYSTEM_STATE_STARTING,
  SYSTEM_STATE_RUNNING,
  SYSTEM_STATE_PAUSED,
  SYSTEM_STATE_STOPPING,
} system_state_t;

typedef enum : u8 {
  SYSTEM_BOOT_MODE_NONE = 0,
  SYSTEM_BOOT_MODE_FULL_BOOT,
  SYSTEM_BOOT_MODE_FAST_BOOT,
  SYSTEM_BOOT_MODE_BOOT_EXE,
  SYSTEM_BOOT_MODE_BOOT_PSF,
  SYSTEM_BOOT_MODE_REPLAY_GPU_DUMP, /* unused in cupid-ps1; kept for ABI */
} system_boot_mode_t;

typedef enum : u8 {
  SYSTEM_TAINT_CPU_OVERCLOCK = 0,
  SYSTEM_TAINT_CDROM_READ_SPEEDUP,
  SYSTEM_TAINT_CDROM_SEEK_SPEEDUP,
  SYSTEM_TAINT_FORCE_FRAME_TIMINGS,
  SYSTEM_TAINT_RAM_8MB,
  SYSTEM_TAINT_CHEATS,
  SYSTEM_TAINT_PATCHES,
  SYSTEM_TAINT_MEMORY_CARD_MISMATCH,
  SYSTEM_TAINT_MAX_COUNT,
} system_taint_t;

typedef struct {
  /* Optional (has=false => default). */
  bool has;
  bool val;
} system_optional_bool_t;

/* All char* fields are heap-owned NUL-terminated; NULL == empty.  Caller-
 * populated via system_boot_parameters_init() then move into system_boot_system. */
typedef struct {
  char* path;             /* primary boot image: cue / exe / psf */
  char* save_state;       /* optional save-state path to restore after boot */
  char* override_exe;     /* optional executable to inject after BIOS */
  u32   media_playlist_index;

  system_optional_bool_t override_fast_boot;
  system_optional_bool_t override_fullscreen;
  system_optional_bool_t override_start_paused;

  bool load_image_to_ram;
  bool ignore_missing_subchannel;
  bool force_software_renderer;
  bool disable_achievements_hardcore_mode;
  bool start_media_capture;
} system_boot_parameters_t;

void system_boot_parameters_init   (system_boot_parameters_t* p);
void system_boot_parameters_destroy(system_boot_parameters_t* p);

typedef struct {
  char*   path;       /* heap, NUL-terminated */
  time_t  timestamp;
  s32     slot;
  bool    global;
} system_save_state_info_t;

void system_save_state_info_destroy(system_save_state_info_t* ssi);

typedef struct {
  char*   title;       /* heap */
  char*   serial;      /* heap */
  char*   media_path;  /* heap */
  time_t  timestamp;

  /* RGBA8 thumbnail pixels.  (NULL when the state had no screenshot.) */
  u8*     screenshot_pixels;
  u32     screenshot_width;
  u32     screenshot_height;
} system_extended_save_state_info_t;

void system_extended_save_state_info_destroy(system_extended_save_state_info_t* essi);

bool system_is_disc_path     (const char* path);
bool system_is_exe_path      (const char* path);
bool system_is_psf_path      (const char* path);
bool system_is_gpu_dump_path (const char* path);
bool system_is_loadable_path (const char* path);
bool system_is_save_state_path(const char* path);

console_region_t system_get_console_region_for_disc_region(disc_region_t region);
disc_region_t    system_get_region_for_serial             (const char* serial);
disc_region_t    system_get_region_from_system_area       (cd_image_t* cdi);
disc_region_t    system_get_region_for_image              (cd_image_t* cdi);
disc_region_t    system_get_region_for_exe                (const char* path);

/* Hashes the executable contained in the cue image for game-id derivation.
 * out_executable_name (when non-NULL) is malloc'd; caller frees. */
bool      system_get_game_details_from_image(cd_image_t* cdi, char** out_id_or_null,
                                               game_hash_t* out_hash_or_null,
                                              char** out_executable_name_or_null, 
                                              u8** out_executable_data_or_null,
                                              size_t* out_executable_size_or_null);
game_hash_t system_get_game_hash_from_file(const char* path);

/* "HASH-{:X}" formatted into out (caller-owned tiny_string_t-equivalent buffer). */
void system_get_game_hash_id(game_hash_t hash, char* out, size_t out_size);

system_state_t     system_get_state               (void);
bool               system_is_running              (void);
bool               system_is_paused               (void);
bool               system_is_shutdown             (void);
bool               system_is_valid                (void);
bool               system_is_valid_or_initializing(void);
bool               system_is_executing            (void);

bool system_is_startup_cancelled (void);
void system_cancel_pending_startup(void);
void system_interrupt_execution  (void);

console_region_t system_get_region    (void);
bool             system_is_pal_region (void);

/* taint flags - cleared on reset. */
const char* system_get_taint_name(system_taint_t taint);
bool        system_has_taint     (system_taint_t taint);
void        system_set_taint     (system_taint_t taint);

ALWAYS_INLINE tick_count_t system_scale_ticks_to_overclock(tick_count_t ticks)
{
  if (!g_settings.cpu_overclock_active)
    return ticks;
  return (tick_count_t)(((u64)(u32)ticks * g_settings.cpu_overclock_numerator +
                         (g_settings.cpu_overclock_denominator - 1u)) /
                        g_settings.cpu_overclock_denominator);
}

ALWAYS_INLINE tick_count_t system_unscale_ticks_to_overclock(tick_count_t ticks, tick_count_t* remainder)
{
  if (!g_settings.cpu_overclock_active)
    return ticks;
  const u64 num =
    ((u64)(u32)ticks * (u64)g_settings.cpu_overclock_denominator) + (u64)(u32)(*remainder);
  const tick_count_t t = (tick_count_t)(u32)(num / g_settings.cpu_overclock_numerator);
  *remainder = (tick_count_t)(u32)(num % g_settings.cpu_overclock_numerator);
  return t;
}

tick_count_t   system_get_ticks_per_second(void);
tick_count_t   system_get_max_slice_ticks (void);
void           system_update_overclock    (void);

global_ticks_t system_get_global_tick_counter(void);
u32            system_get_frame_number       (void);
u32            system_get_internal_frame_number(void);

/* All const char* getters return non-NULL pointers ("" when empty). */
const char*    system_get_game_title (void);
const char*    system_get_game_serial(void);
const char*    system_get_game_path  (void);
const char*    system_get_exe_override(void);

/* Called from bus.c kernel_initialized_hook (BIOS POST=07).  When boot_mode
 * is BOOT_EXE, copies the held EXE buffer into RAM and seeds CPU pc/gp/sp
 * via bus_inject_executable.  No-op for any other boot mode. */
void           system_handle_kernel_initialized(void);

game_hash_t        system_get_game_hash    (void);
bool               system_is_running_unknown_game(void);
bool               system_is_using_known_ps1_bios(void);
system_boot_mode_t system_get_boot_mode    (void);

/* Returns the GameDB entry associated with the disc currently booted, or NULL
 * if the disc had no serial / no GameDB hit / no disc was booted (BIOS/EXE/
 * PSF).  The pointer is owned by the GameDB loader and remains valid for the
 * lifetime of the process. */
const game_database_entry_t* system_get_game_database_entry(void);

void system_load_settings              (bool display_osd_messages);
void system_apply_settings             (bool display_osd_messages);
void system_reload_input_sources       (void);
void system_reload_input_bindings      (void);
void system_update_controller_settings (void);

bool system_boot_system   (system_boot_parameters_t* parameters, Error* error);
void system_pause_system  (bool paused);
void system_reset_system  (void);
void system_shutdown_system(bool save_resume_state);

size_t system_get_max_save_state_size(bool enable_8mb_ram);

/* Returns: 1 = success, 0 = failure (error populated), -1 = deferred (e.g. UI
 * confirm).  The original std::optional<bool> collapses to this trit. */
int  system_load_state(const char* path, Error* error, bool save_undo_state, bool force_update_display);
bool system_save_state(const char* path, Error* error, bool backup_existing_save, bool ignore_memcard_busy);
bool system_save_resume_state(Error* error);

void system_load_state_from_slot(bool global, s32 slot);
void system_save_state_to_slot  (bool global, s32 slot);

void system_execute       (void); /* runs CPU until execution cancelled */
void system_single_step_cpu(void);

/* speed limiter / target speed */
float system_get_target_speed       (void);
float system_get_audio_nominal_rate (void);
bool  system_is_running_at_non_standard_speed(void);
float system_get_video_frame_rate(void);
void  system_set_video_frame_rate(float frequency);

void  system_update_speed_limiter_state(void);

bool  system_is_fast_forward_enabled(void);
void  system_set_fast_forward_enabled(bool enabled);
bool  system_is_turbo_enabled        (void);
void  system_set_turbo_enabled       (bool enabled);

void  system_do_frame_step(void);

controller_t* system_get_controller(u32 slot);
void          system_update_memory_cards(void);
bool          system_has_memory_card(u32 slot);
bool          system_is_saving_memory_cards(void);
void          system_swap_memory_cards(void);

bool system_dump_ram   (const char* path, Error* error);
bool system_dump_spu_ram(const char* path, Error* error);

bool system_insert_media(const char* path);
void system_remove_media(void);

/* Multi-disc / sub-image (m3u playlist) API.  Read-only queries return
 * 0/false/NULL when no media is inserted or the current image is single-
 * disc.  system_switch_media_sub_image performs a disc-swap dance through
 * cdrom_remove_media/cdrom_insert_media so the game sees a proper shell-
 * open / disc-changed cycle. */
bool  system_has_media_sub_images       (void);
u32   system_get_media_sub_image_count  (void);
u32   system_get_media_sub_image_index  (void);
char* system_get_media_sub_image_title  (u32 index);  /* heap; caller frees */
bool  system_switch_media_sub_image     (u32 index, Error* error);

void system_get_game_save_state_path(const char* serial, s32 slot, small_string_t* out);
void system_get_global_save_state_path(s32 slot, small_string_t* out);
void system_get_most_recent_resume_save_state_path(small_string_t* out);

void system_flush_save_states(void);

void system_increment_frame_number          (void);
void system_increment_internal_frame_number (void);
void system_frame_done                      (void);

void system_on_memory_card_accessed(void);

void system_abnormal_shutdown(const char* reason);

/* Process / core-thread lifecycle (called by frontend). */
bool system_process_startup        (Error* error);
void system_process_shutdown       (void);
bool system_core_thread_initialize (Error* error);
void system_core_thread_shutdown   (void);

void system_idle_poll_update(void);

/* Frontend-provided: requests a clean shutdown of the running VM. */
void host_request_system_shutdown(bool allow_confirm, bool save_state, bool check_memcard_busy);

#endif /* CUPID_CORE_SYSTEM_H */
