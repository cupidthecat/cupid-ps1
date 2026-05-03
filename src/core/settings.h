/*
 * fields exposed.  Bitfield members lose their `:1` packing (saves space at
 * the cost of cache: still <1 KiB total) so plain `bool` is fine.
 *
 * Serialization mapping:
 *   void Settings::Load(const SI& si, const SI& controller_si)
 *     -> void settings_load(settings_t* s, settings_interface_t* si,
 *                           settings_interface_t* controller_si);
 *   void Settings::Save(SI& si, bool ignore_base) const
 *     -> void settings_save(const settings_t* s, settings_interface_t* si,
 *                           bool ignore_base);
 *
 * Per-game cheats / hotkeys / achievements config is *stored* in the struct
 * (so callers can configure it later) but `settings_load` / `settings_save`
 * skip the achievements + per-controller binding sections - the input/cheevo
 * subsystems are deferred and pull in too many dependencies to load right
 * now.  See "// achievements: not loaded yet" markers in the .c.
 *
 * std::string fields are heap `char*`; settings_init() / settings_destroy()
 * own them.  Memory card path entries are similarly owned.
 */

#ifndef CUPID_CORE_SETTINGS_H
#define CUPID_CORE_SETTINGS_H

#include "common/log.h"
#include "common/settings_interface.h"
#include "common/types.h"
#include "util/audio_stream.h"
#include "core/types.h"

#include <stddef.h>

typedef struct {
  audio_stretch_mode_t stretch_mode;
  bool                 output_latency_minimal;
  u16                  output_latency_ms;
  u16                  buffer_ms;
  u16                  stretch_sequence_length_ms;
  u16                  stretch_seekwindow_ms;
  u16                  stretch_overlap_ms;
  bool                 stretch_use_quickseek;
  bool                 stretch_use_aa_filter;
} settings_audio_stream_parameters_t;

#define SETTINGS_AUDIO_DEFAULT_STRETCH_MODE              AUDIO_STRETCH_MODE_TIME
#define SETTINGS_AUDIO_DEFAULT_BUFFER_MS                 50
#define SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MS         20
#define SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MINIMAL    false
#define SETTINGS_AUDIO_DEFAULT_STRETCH_SEQUENCE_LENGTH   30
#define SETTINGS_AUDIO_DEFAULT_STRETCH_SEEKWINDOW        20
#define SETTINGS_AUDIO_DEFAULT_STRETCH_OVERLAP           10
#define SETTINGS_AUDIO_DEFAULT_STRETCH_USE_QUICKSEEK     false
#define SETTINGS_AUDIO_DEFAULT_STRETCH_USE_AA_FILTER     false

enum {
  SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_ENTRIES         = 1200,
  SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB   = 2048,
  SETTINGS_TR_DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_MB  = 512,
};

typedef struct {
  u32 max_hash_cache_entries;
  u32 max_hash_cache_vram_usage_mb;
  u32 max_replacement_cache_vram_usage_mb;

  u16 max_vram_write_splits;
  u16 max_vram_write_coalesce_width;
  u16 max_vram_write_coalesce_height;
  u16 texture_dump_width_threshold;
  u16 texture_dump_height_threshold;

  u16 vram_write_dump_width_threshold;
  u16 vram_write_dump_height_threshold;

  bool dump_texture_pages;
  bool dump_full_texture_pages;
  bool dump_texture_force_alpha_channel;
  bool dump_vram_write_force_alpha_channel;
  bool dump_c16_textures;
  bool reduce_palette_range;
  bool convert_copies_to_writes;
  bool replacement_scale_linear_filter;
} settings_texture_replacement_config_t;

typedef struct {
  bool enable_texture_replacements;
  bool enable_vram_write_replacements;
  bool always_track_uploads;
  bool preload_textures;

  bool dump_textures;
  bool dump_replaced_textures;
  bool dump_vram_writes;

  settings_texture_replacement_config_t config;
} settings_texture_replacement_t;

#define SETTINGS_DEFAULT_GPU_RENDERER                 GPU_RENDERER_SOFTWARE
#define SETTINGS_DEFAULT_GPU_TEXTURE_FILTER           GPU_TEXTURE_FILTER_NEAREST
#define SETTINGS_DEFAULT_GPU_DITHERING_MODE           GPU_DITHERING_MODE_TRUE_COLOR
#define SETTINGS_DEFAULT_GPU_LINE_DETECT_MODE         GPU_LINE_DETECT_MODE_DISABLED
#define SETTINGS_DEFAULT_GPU_DOWNSAMPLE_MODE          GPU_DOWNSAMPLE_MODE_DISABLED
#define SETTINGS_DEFAULT_GPU_WIREFRAME_MODE           GPU_WIREFRAME_MODE_DISABLED
#define SETTINGS_DEFAULT_GPU_DUMP_COMPRESSION_MODE    GPU_DUMP_COMPRESSION_MODE_ZST_DEFAULT
#define SETTINGS_DEFAULT_GPU_PGXP_DEPTH_THRESHOLD     4096.0f
#define SETTINGS_GPU_PGXP_DEPTH_THRESHOLD_SCALE       65536.0f

#define SETTINGS_DEFAULT_DISPLAY_DEINTERLACING_MODE   DISPLAY_DEINTERLACING_MODE_PROGRESSIVE
#define SETTINGS_DEFAULT_DISPLAY_CROP_MODE            DISPLAY_CROP_MODE_OVERSCAN
#define SETTINGS_DEFAULT_DISPLAY_FINE_CROP_MODE       DISPLAY_FINE_CROP_MODE_NONE
#define SETTINGS_DEFAULT_DISPLAY_ALIGNMENT            DISPLAY_ALIGNMENT_CENTER
#define SETTINGS_DEFAULT_DISPLAY_ROTATION             DISPLAY_ROTATION_NORMAL
#define SETTINGS_DEFAULT_DISPLAY_SCALING              DISPLAY_SCALING_MODE_BILINEAR_SMOOTH
#define SETTINGS_DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL_AUTOMATIC
#define SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_MODE      DISPLAY_SCREENSHOT_MODE_SCREEN_RESOLUTION
#define SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_FORMAT    DISPLAY_SCREENSHOT_FORMAT_PNG
#define SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_QUALITY   85
#define SETTINGS_DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUF  2.0f
#define SETTINGS_DEFAULT_OSD_SCALE                    100.0f
#define SETTINGS_DEFAULT_OSD_MARGIN                   10.0f /* matches ImGuiManager::DEFAULT_SCREEN_MARGIN */
#define SETTINGS_DEFAULT_OSD_MESSAGE_LOCATION         NOTIFICATION_LOCATION_TOP_LEFT

#define SETTINGS_DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE \
        ACHIEVEMENT_CHALLENGE_INDICATOR_MODE_NOTIFICATION
#define SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_LOCATION    NOTIFICATION_LOCATION_TOP_LEFT
#define SETTINGS_DEFAULT_ACHIEVEMENT_INDICATOR_LOCATION       NOTIFICATION_LOCATION_BOTTOM_RIGHT
#define SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_OSD_SCALE     (-1)
#define SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_AUTO          0

#define SETTINGS_DEFAULT_GPU_MAX_QUEUED_FRAMES        2
#define SETTINGS_DEFAULT_GPU_PREFER_GLES_CONTEXT      false
#define SETTINGS_DEFAULT_OPTIMAL_FRAME_PACING         true

enum {
  SETTINGS_DEFAULT_DMA_MAX_SLICE_TICKS = 1000,
  SETTINGS_DEFAULT_DMA_HALT_TICKS      = 100,
  SETTINGS_DEFAULT_GPU_FIFO_SIZE       = 16,
  SETTINGS_DEFAULT_GPU_MAX_RUN_AHEAD   = 128,
};
#define SETTINGS_DEFAULT_CONSOLE_REGION             CONSOLE_REGION_AUTO
#define SETTINGS_DEFAULT_FORCE_VIDEO_TIMING_MODE    FORCE_VIDEO_TIMING_MODE_DISABLED
/* x64 recompiler is now the default (M1-M6 closed: mtc0/rfe + SMC + icache
 * + fastmem + PGXP + end-block-and-interpret fallback).  Override via
 * `--cpu-mode interp` for diff-trace harness or hosts without x64. */
#define SETTINGS_DEFAULT_CPU_EXECUTION_MODE         CPU_EXECUTION_MODE_RECOMPILER
/* M5: MMap is the only mode the recompiler emits inline-SIB fastmem
 * sites against (LUT mode falls through to slow path).  Default to MMap
 * so out-of-the-box recomp gets the SIGSEGV-backed path.  Users on hosts
 * where 4 GiB virtual reservation fails can override via --fastmem lut. */
#define SETTINGS_DEFAULT_CPU_FASTMEM_MODE           CPU_FASTMEM_MODE_MMAP
#define SETTINGS_DEFAULT_CDROM_READAHEAD_SECTORS    8
#define SETTINGS_DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES 30000u
#define SETTINGS_DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES 30000u
#define SETTINGS_DEFAULT_CDROM_MECHACON_VERSION     CDROM_MECHACON_VERSION_VC1A
#define SETTINGS_DEFAULT_CONTROLLER_1_TYPE          CONTROLLER_TYPE_ANALOG_CONTROLLER
#define SETTINGS_DEFAULT_CONTROLLER_2_TYPE          CONTROLLER_TYPE_NONE
#define SETTINGS_DEFAULT_MEMORY_CARD_1_TYPE         MEMORY_CARD_TYPE_PER_GAME_TITLE
#define SETTINGS_DEFAULT_MEMORY_CARD_2_TYPE         MEMORY_CARD_TYPE_NONE
#define SETTINGS_DEFAULT_MULTITAP_MODE              MULTITAP_MODE_DISABLED
#define SETTINGS_DEFAULT_PIO_DEVICE_TYPE            PIO_DEVICE_TYPE_NONE
#define SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME 5
#define SETTINGS_DEFAULT_LEADERBOARD_NOTIFICATION_TIME 10
#define SETTINGS_DEFAULT_SAVE_STATE_COMPRESSION_MODE   SAVE_STATE_COMPRESSION_MODE_ZST_DEFAULT
#define SETTINGS_DEFAULT_SAVE_STATE_BACKUPS         true
#define SETTINGS_DEFAULT_FAST_BOOT_VALUE            false
#define SETTINGS_DEFAULT_GDB_SERVER_PORT            2345
#define SETTINGS_DEFAULT_ACHIEVEMENT_BADGE_PREFETCH true

typedef struct {
  gpu_renderer_t                          gpu_renderer;
  u8                                      gpu_resolution_scale;
  u8                                      gpu_multisamples;

  gpu_texture_filter_t                    gpu_texture_filter;
  gpu_texture_filter_t                    gpu_sprite_texture_filter;
  gpu_dithering_mode_t                    gpu_dithering_mode;
  gpu_line_detect_mode_t                  gpu_line_detect_mode;
  gpu_downsample_mode_t                   gpu_downsample_mode;
  u8                                      gpu_downsample_scale;
  gpu_wireframe_mode_t                    gpu_wireframe_mode;
  display_aspect_ratio_t                  display_aspect_ratio;
  display_deinterlacing_mode_t            display_deinterlacing_mode;
  display_crop_mode_t                     display_crop_mode;
  display_fine_crop_mode_t                display_fine_crop_mode;
  display_alignment_t                     display_alignment;
  display_rotation_t                      display_rotation;
  display_scaling_mode_t                  display_scaling;
  display_scaling_mode_t                  display_scaling_24bit;
  display_exclusive_fullscreen_control_t  display_exclusive_fullscreen_control;
  display_screenshot_mode_t               display_screenshot_mode;
  display_screenshot_format_t             display_screenshot_format;
  u8                                      display_screenshot_quality;
  u8                                      gpu_max_queued_frames;
  s16                                     display_active_start_offset;
  s16                                     display_active_end_offset;
  s8                                      display_line_start_offset;
  s8                                      display_line_end_offset;

  bool gpu_use_thread;
  bool gpu_use_software_renderer_for_readbacks;
  bool gpu_use_software_renderer_for_memory_states;
  bool gpu_use_debug_device;
  bool gpu_use_debug_device_gpu_validation;
  bool gpu_prefer_gles_context;
  bool gpu_disable_shader_cache;
  bool gpu_disable_dual_source_blend;
  bool gpu_disable_framebuffer_fetch;
  bool gpu_disable_texture_buffers;
  bool gpu_disable_texture_copy_to_self;
  bool gpu_disable_memory_import;
  bool gpu_disable_raster_order_views;
  bool gpu_disable_compute_shaders;
  bool gpu_disable_compressed_textures;
  bool gpu_automatic_resolution_scale;
  bool gpu_per_sample_shading;
  bool gpu_scaled_interlacing;
  bool gpu_force_round_texcoords;
  bool gpu_widescreen_rendering;
  bool gpu_widescreen_hack;
  bool gpu_modulation_crop;
  bool gpu_texture_cache;
  bool gpu_show_vram;
  bool gpu_dump_cpu_to_vram_copies;
  bool gpu_dump_vram_to_cpu_copies;
  bool gpu_dump_fast_replay_mode;

  bool gpu_pgxp_enable;
  bool gpu_pgxp_culling;
  bool gpu_pgxp_texture_correction;
  bool gpu_pgxp_color_correction;
  bool gpu_pgxp_vertex_cache;
  bool gpu_pgxp_cpu;
  bool gpu_pgxp_preserve_proj_fp;
  bool gpu_pgxp_depth_buffer;
  bool gpu_pgxp_disable_2d;
  bool gpu_pgxp_transparent_depth;

  bool display_optimal_frame_pacing;
  bool display_pre_frame_sleep;
  bool display_skip_presenting_duplicate_frames;
  bool display_vsync;
  bool display_disable_mailbox_presentation;
  bool display_force_4_3_for_24bit;
  bool display_24bit_chroma_smoothing;
  bool display_show_messages;
  bool display_animate_messages;
  bool display_blur_message_backgrounds;
  bool display_show_fps;
  bool display_show_speed;
  bool display_show_gpu_stats;
  bool display_show_resolution;
  bool display_show_latency_stats;
  bool display_show_cpu_usage;
  bool display_show_gpu_usage;
  bool display_show_frame_times;
  bool display_show_status_indicators;
  bool display_show_inputs;
  bool display_show_enhancements;
  bool display_auto_resize_window;

  float gpu_pgxp_tolerance;
  /* Stored pre-divided by GTE::MAX_Z (65536) for compact internal
   * representation; settings_get_pgxp_depth_clear_threshold() multiplies back. */
  float gpu_pgxp_depth_clear_threshold;

  s16 display_fine_crop_amount[4];

  float display_osd_scale;
  float display_osd_margin;

  /* Indexed by OSDMessageType { Error, Warning, Info, Quick }. */
  float display_osd_message_duration[4];
  notification_location_t display_osd_message_location;

  /* achievements presentation block (loaded with the rest, but the actual
   * RetroAchievements integration is deferred). */
  notification_location_t                achievements_notification_location;
  notification_location_t                achievements_indicator_location;
  achievement_challenge_indicator_mode_t achievements_challenge_indicator_mode;
  s16                                    achievements_notification_scale;
  s16                                    achievements_indicator_scale;

  settings_texture_replacement_t texture_replacements;

  /* heap-owned, may be NULL or "" */
  char* overlay_image_path;

  u32 cpu_overclock_numerator;
  u32 cpu_overclock_denominator;

  tick_count_t dma_max_slice_ticks;
  tick_count_t dma_halt_ticks;
  u32          gpu_fifo_size;
  tick_count_t gpu_max_run_ahead;

  console_region_t           region;
  force_video_timing_mode_t  gpu_force_video_timing;

  cpu_execution_mode_t cpu_execution_mode;
  cpu_fastmem_mode_t   cpu_fastmem_mode;
  bool cpu_overclock_enable;
  bool cpu_overclock_active;
  bool cpu_recompiler_memory_exceptions;
  bool cpu_recompiler_block_linking;
  bool cpu_recompiler_icache;
  bool cpu_recompiler_compare_register_files; /* per-block recomp/interp diff */
  bool cpu_enable_8mb_ram;

  bool mdec_use_old_routines;
  bool mdec_disable_cdrom_speedup;

  bool pcdrv_enable;
  bool pcdrv_enable_writes;

  bool pio_switch_active;
  bool pio_flash_write_enable;

  bool sio_redirect_to_tty;

  bool memory_card_use_playlist_title;
  bool memory_card_fast_forward_access;

  bool cdrom_region_check;
  bool cdrom_subq_skew;
  bool cdrom_load_image_to_ram;
  bool cdrom_load_image_patches;
  bool cdrom_ignore_host_subcode;
  bool cdrom_mute_cd_audio;
  bool cdrom_auto_disc_change;

  bool bios_tty_logging;
  bool bios_patch_fast_boot;
  bool bios_fast_forward_boot;

  bool rewind_enable;
  bool runahead_for_analog_input;

  bool apply_compatibility_settings;
  bool apply_game_settings;
  bool load_devices_from_save_states;

  u16 rewind_save_slots;
  u8  runahead_frames;

  save_state_compression_mode_t save_state_compression;

  u8                       cdrom_readahead_sectors;
  cdrom_mechacon_version_t cdrom_mechacon_version;

  u8  cdrom_read_speedup;
  u8  cdrom_seek_speedup;
  u32 cdrom_max_seek_speedup_cycles;
  u32 cdrom_max_read_speedup_cycles;

  u8 audio_output_volume;
  u8 audio_fast_forward_volume;

  bool audio_output_muted;

  bool sync_to_host_refresh_rate;
  bool inhibit_screensaver;
  bool pause_on_focus_loss;
  bool pause_on_controller_disconnection;
  bool disable_background_input;
  bool save_state_on_exit;
  bool create_save_state_backups;
  bool confim_power_off;          /* original misspelling preserved */
  bool disable_all_enhancements;
  bool enable_discord_presence;
  bool export_shared_memory;

  /* achievements config bits (loaded but the system itself is deferred). */
  bool achievements_enabled;
  bool achievements_hardcore_mode;
  bool achievements_encore_mode;
  bool achievements_spectator_mode;
  bool achievements_unofficial_test_mode;
  bool achievements_use_raintegration;
  bool achievements_notifications;
  bool achievements_leaderboard_notifications;
  bool achievements_leaderboard_trackers;
  bool achievements_sound_effects;
  bool achievements_progress_indicators;
  bool achievements_prefetch_badges;
  u8   achievements_notification_duration;
  u8   achievements_leaderboard_duration;

  float emulation_speed;
  float fast_forward_speed;
  float turbo_speed;

  float rewind_save_frequency;

  float display_pre_frame_sleep_buffer;

  controller_type_t  controller_types[NUM_CONTROLLER_AND_CARD_PORTS];
  memory_card_type_t memory_card_types[NUM_CONTROLLER_AND_CARD_PORTS];
  /* Heap-owned strings; NULL means "no override". */
  char*              memory_card_paths[NUM_CONTROLLER_AND_CARD_PORTS];

  multitap_mode_t   multitap_mode;
  pio_device_type_t pio_device_type;

  audio_backend_t                    audio_backend;
  settings_audio_stream_parameters_t audio_stream_parameters;

  char* gpu_adapter;
  char* gpu_sw_use_isa;     /* "" auto, "AVX2", "SCALAR".  Picked at startup. */
  char* audio_driver;
  char* audio_output_device;

  char* pio_flash_image_path;
  char* pcdrv_root;

  u16  gdb_server_port;
  bool enable_gdb_server;
} settings_t;

/* Initialises every field to its default value (matches Settings::Settings()
 * + GPUSettings::GPUSettings()).  All char* members are set to NULL so
 * settings_destroy() can free unconditionally. */
void settings_init(settings_t* s);

/* Frees every char* member.  Tolerates partial init. */
void settings_destroy(settings_t* s);

/* `controller_si` provides the controller type entries.  Pass the same
 * pointer as `si` if there's no separate input layer. */
void settings_load(settings_t* s, settings_interface_t* si,
                   settings_interface_t* controller_si);
void settings_load_pgxp(settings_t* s, settings_interface_t* si);
void settings_save(const settings_t* s, settings_interface_t* si, bool ignore_base);

void settings_apply_setting_restrictions(settings_t* s);

/* Returns the multiplied threshold (multiplied by GTE MAX_Z = 65536). */
float settings_get_pgxp_depth_clear_threshold(const settings_t* s);
void  settings_set_pgxp_depth_clear_threshold(settings_t* s, float value);

bool settings_is_using_software_renderer(const settings_t* s);
bool settings_is_using_true_color(const settings_t* s);
bool settings_is_using_dithering(const settings_t* s);
bool settings_is_using_shader_blending(const settings_t* s);
bool settings_is_using_scaled_dithering(const settings_t* s);
bool settings_is_using_integer_display_scaling(const settings_t* s, bool is_24bit);
bool settings_using_pgxp_cpu_mode(const settings_t* s);
bool settings_using_pgxp_depth_buffer(const settings_t* s);

bool settings_is_runahead_enabled(const settings_t* s);
u8   settings_get_audio_output_volume(const settings_t* s, bool fast_forwarding);
bool settings_is_port1_multitap_enabled(const settings_t* s);
bool settings_is_port2_multitap_enabled(const settings_t* s);
bool settings_is_multitap_port_enabled(const settings_t* s, u32 port);

controller_type_t settings_get_default_controller_type(u32 pad);
bool              settings_is_per_game_memory_card_type(memory_card_type_t type);
bool              settings_has_any_per_game_memory_cards(const settings_t* s);

void settings_cpu_overclock_percent_to_fraction(u32 percent, u32* numerator, u32* denominator);
u32  settings_cpu_overclock_fraction_to_percent(u32 numerator, u32 denominator);
void settings_set_cpu_overclock_percent(settings_t* s, u32 percent);
u32  settings_get_cpu_overclock_percent(const settings_t* s);
void settings_update_overclock_active(settings_t* s);

/* GPU device-relevant change check (subset of fields the renderer cares
 * about).  Useful for "do we have to recreate the device?" gating. */
bool settings_are_gpu_device_settings_changed(const settings_t* a, const settings_t* b);

/* Returns a pointer to a static array of section name pointers in canonical
 * save order; *out_count is set to the number of entries.  The names match
 * the section strings used by settings_save(). */
const char* const* settings_get_section_save_order(size_t* out_count);

bool        settings_parse_log_level_name(const char* str, log_level_t* out);
const char* settings_get_log_level_name(log_level_t level);
const char* settings_get_log_level_display_name(log_level_t level);

bool        settings_parse_console_region_name(const char* str, console_region_t* out);
const char* settings_get_console_region_name(console_region_t region);
const char* settings_get_console_region_display_name(console_region_t region);

bool        settings_parse_disc_region_name(const char* str, disc_region_t* out);
const char* settings_get_disc_region_name(disc_region_t region);
const char* settings_get_disc_region_display_name(disc_region_t region);

bool        settings_parse_cpu_execution_mode_name(const char* str, cpu_execution_mode_t* out);
const char* settings_get_cpu_execution_mode_name(cpu_execution_mode_t mode);
const char* settings_get_cpu_execution_mode_display_name(cpu_execution_mode_t mode);

bool        settings_parse_cpu_fastmem_mode_name(const char* str, cpu_fastmem_mode_t* out);
const char* settings_get_cpu_fastmem_mode_name(cpu_fastmem_mode_t mode);
const char* settings_get_cpu_fastmem_mode_display_name(cpu_fastmem_mode_t mode);

bool        settings_parse_renderer_name(const char* str, gpu_renderer_t* out);
const char* settings_get_renderer_name(gpu_renderer_t renderer);
const char* settings_get_renderer_display_name(gpu_renderer_t renderer);

bool        settings_parse_texture_filter_name(const char* str, gpu_texture_filter_t* out);
const char* settings_get_texture_filter_name(gpu_texture_filter_t filter);
const char* settings_get_texture_filter_display_name(gpu_texture_filter_t filter);

bool        settings_parse_gpu_dithering_mode_name(const char* str, gpu_dithering_mode_t* out);
const char* settings_get_gpu_dithering_mode_name(gpu_dithering_mode_t mode);
const char* settings_get_gpu_dithering_mode_display_name(gpu_dithering_mode_t mode);

bool        settings_parse_line_detect_mode_name(const char* str, gpu_line_detect_mode_t* out);
const char* settings_get_line_detect_mode_name(gpu_line_detect_mode_t mode);
const char* settings_get_line_detect_mode_display_name(gpu_line_detect_mode_t mode);

bool        settings_parse_downsample_mode_name(const char* str, gpu_downsample_mode_t* out);
const char* settings_get_downsample_mode_name(gpu_downsample_mode_t mode);
const char* settings_get_downsample_mode_display_name(gpu_downsample_mode_t mode);

bool        settings_parse_gpu_wireframe_mode_name(const char* str, gpu_wireframe_mode_t* out);
const char* settings_get_gpu_wireframe_mode_name(gpu_wireframe_mode_t mode);
const char* settings_get_gpu_wireframe_mode_display_name(gpu_wireframe_mode_t mode);

bool        settings_parse_gpu_dump_compression_mode_name(const char* str, gpu_dump_compression_mode_t* out);
const char* settings_get_gpu_dump_compression_mode_name(gpu_dump_compression_mode_t mode);
const char* settings_get_gpu_dump_compression_mode_display_name(gpu_dump_compression_mode_t mode);

bool        settings_parse_display_deinterlacing_mode_name(const char* str, display_deinterlacing_mode_t* out);
const char* settings_get_display_deinterlacing_mode_name(display_deinterlacing_mode_t mode);
const char* settings_get_display_deinterlacing_mode_display_name(display_deinterlacing_mode_t mode);

bool        settings_parse_display_crop_mode_name(const char* str, display_crop_mode_t* out);
const char* settings_get_display_crop_mode_name(display_crop_mode_t mode);
const char* settings_get_display_crop_mode_display_name(display_crop_mode_t mode);

bool        settings_parse_display_fine_crop_mode_name(const char* str, display_fine_crop_mode_t* out);
const char* settings_get_display_fine_crop_mode_name(display_fine_crop_mode_t mode);
const char* settings_get_display_fine_crop_mode_display_name(display_fine_crop_mode_t mode);

/* Aspect-ratio name parsing handles "Auto (Game Native)" / "Stretch To Fill" /
 * "PAR 1:1" + "n:d" forms. *out_str_size is the destination buffer size for
 * the textual name; output is truncated if too small. */
bool settings_parse_display_aspect_ratio(const char* str, display_aspect_ratio_t* out);
void settings_get_display_aspect_ratio_name(display_aspect_ratio_t ar, char* out, size_t out_size);
void settings_get_display_aspect_ratio_display_name(display_aspect_ratio_t ar, char* out, size_t out_size);
const display_aspect_ratio_t* settings_get_predefined_display_aspect_ratios(size_t* out_count);

bool        settings_parse_display_alignment_name(const char* str, display_alignment_t* out);
const char* settings_get_display_alignment_name(display_alignment_t alignment);
const char* settings_get_display_alignment_display_name(display_alignment_t alignment);

bool        settings_parse_display_rotation_name(const char* str, display_rotation_t* out);
const char* settings_get_display_rotation_name(display_rotation_t rotation);
const char* settings_get_display_rotation_display_name(display_rotation_t rotation);

bool        settings_parse_display_scaling_name(const char* str, display_scaling_mode_t* out);
const char* settings_get_display_scaling_name(display_scaling_mode_t mode);
const char* settings_get_display_scaling_display_name(display_scaling_mode_t mode);

bool        settings_parse_force_video_timing_name(const char* str, force_video_timing_mode_t* out);
const char* settings_get_force_video_timing_name(force_video_timing_mode_t mode);
const char* settings_get_force_video_timing_display_name(force_video_timing_mode_t mode);

bool        settings_parse_display_exclusive_fullscreen_control_name(const char* str,
                                                                      display_exclusive_fullscreen_control_t* out);
const char* settings_get_display_exclusive_fullscreen_control_name(display_exclusive_fullscreen_control_t mode);
const char* settings_get_display_exclusive_fullscreen_control_display_name(display_exclusive_fullscreen_control_t mode);

bool        settings_parse_display_screenshot_mode_name(const char* str, display_screenshot_mode_t* out);
const char* settings_get_display_screenshot_mode_name(display_screenshot_mode_t mode);
const char* settings_get_display_screenshot_mode_display_name(display_screenshot_mode_t mode);

bool        settings_parse_display_screenshot_format_name(const char* str, display_screenshot_format_t* out);
const char* settings_get_display_screenshot_format_name(display_screenshot_format_t format);
const char* settings_get_display_screenshot_format_display_name(display_screenshot_format_t format);
const char* settings_get_display_screenshot_format_extension(display_screenshot_format_t format);
bool        settings_get_display_screenshot_format_from_filename(const char* filename,
                                                                  display_screenshot_format_t* out);

bool        settings_parse_notification_location_name(const char* str, notification_location_t* out);
const char* settings_get_notification_location_name(notification_location_t location);
const char* settings_get_notification_location_display_name(notification_location_t location);

bool        settings_parse_achievement_challenge_indicator_mode_name(const char* str,
                                                                      achievement_challenge_indicator_mode_t* out);
const char* settings_get_achievement_challenge_indicator_mode_name(achievement_challenge_indicator_mode_t mode);
const char* settings_get_achievement_challenge_indicator_mode_display_name(achievement_challenge_indicator_mode_t mode);

bool        settings_parse_memory_card_type_name(const char* str, memory_card_type_t* out);
const char* settings_get_memory_card_type_name(memory_card_type_t type);
const char* settings_get_memory_card_type_display_name(memory_card_type_t type);

bool        settings_parse_multitap_mode_name(const char* str, multitap_mode_t* out);
const char* settings_get_multitap_mode_name(multitap_mode_t mode);
const char* settings_get_multitap_mode_display_name(multitap_mode_t mode);

bool        settings_parse_cdrom_mech_version_name(const char* str, cdrom_mechacon_version_t* out);
const char* settings_get_cdrom_mech_version_name(cdrom_mechacon_version_t mode);
const char* settings_get_cdrom_mech_version_display_name(cdrom_mechacon_version_t mode);

bool        settings_parse_save_state_compression_mode_name(const char* str,
                                                             save_state_compression_mode_t* out);
const char* settings_get_save_state_compression_mode_name(save_state_compression_mode_t mode);
const char* settings_get_save_state_compression_mode_display_name(save_state_compression_mode_t mode);

bool        settings_parse_pio_device_type_name(const char* str, pio_device_type_t* out);
const char* settings_get_pio_device_type_name(pio_device_type_t type);
const char* settings_get_pio_device_type_display_name(pio_device_type_t type);

const char* settings_get_display_osd_message_type_name(u32 type);

void settings_get_controller_settings_section(u32 pad, char* out, size_t out_size);

/* (port, slot) decomposition of a flat pad index. */
void settings_pad_to_port_and_slot(u32 pad, u32* out_port, u32* out_slot);
bool settings_pad_is_multitap_slot(u32 pad);

extern ALIGN_TO_CACHE_LINE settings_t g_settings;
extern ALIGN_TO_CACHE_LINE settings_t g_gpu_settings;

/* Thin accessor used by bus.c (which intentionally does not include all of
 * settings.h's heavy struct).  Returns g_settings.cpu_enable_8mb_ram. */
bool settings_get_cpu_enable_8mb_ram(void);

#endif /* CUPID_CORE_SETTINGS_H */
