/*
 * Settings::Save, but skip a few sections that pull in subsystems we have
 * not yet ported:
 *   - achievements (Cheevos): the *fields* are loaded for round-trip but no
 *     side-effects (Achievements::* / RAIntegration calls) happen.
 *   - per-controller bindings (Pad1..Pad8): only the controller *type* slot
 *     is read, because Controller::GetSettingsSection / GetControllerInfo
 *     belong to the input layer.
 *   - hotkeys: not touched at all yet.
 */

#include "core/settings.h"

#include "common/assert.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "common/small_string.h"
#include "common/string_util.h"
#include "common/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Settings);

ALIGN_TO_CACHE_LINE settings_t g_settings;
ALIGN_TO_CACHE_LINE settings_t g_gpu_settings;

bool settings_get_cpu_enable_8mb_ram(void)
{
  return g_settings.cpu_enable_8mb_ram;
}

/* Frees *slot and replaces it with a strdup of new_value (NULL-safe both
 * directions; empty new_value still becomes a NULL slot, equivalent to a
 * default-constructed std::string). */
static void settings_set_string(char** slot, const char* new_value)
{
  free(*slot);
  *slot = (new_value && new_value[0]) ? strdup(new_value) : NULL;
}

/* Reads a string setting via the interface and assigns into *slot.  Empty
 * key value yields a NULL slot. */
static void settings_load_string(char** slot, const settings_interface_t* si,
                                 const char* section, const char* key)
{
  small_string_t buf;
  small_string_init(&buf);
  if (settings_interface_lookup_value(si, section, key, &buf))
    settings_set_string(slot, small_string_c_str(&buf));
  else
    settings_set_string(slot, NULL);
  small_string_destroy(&buf);
}

/* Returns a NUL-terminated view of *slot ("" if NULL). */
static const char* settings_str(const char* slot) { return slot ? slot : ""; }

 /*
 * the projection scale); the PGXP threshold is normalised against this. */
#define SETTINGS_GTE_MAX_Z_F 65536.0f

float settings_get_pgxp_depth_clear_threshold(const settings_t* s)
{
  return s->gpu_pgxp_depth_clear_threshold * SETTINGS_GTE_MAX_Z_F;
}

void settings_set_pgxp_depth_clear_threshold(settings_t* s, float value)
{
  s->gpu_pgxp_depth_clear_threshold = value / SETTINGS_GTE_MAX_Z_F;
}

void settings_init(settings_t* s)
{
  memset(s, 0, sizeof(*s));

  /* GPU/display defaults */
  s->gpu_renderer                = SETTINGS_DEFAULT_GPU_RENDERER;
  s->gpu_resolution_scale        = 1;
  s->gpu_multisamples            = 1;
  s->gpu_texture_filter          = SETTINGS_DEFAULT_GPU_TEXTURE_FILTER;
  s->gpu_sprite_texture_filter   = SETTINGS_DEFAULT_GPU_TEXTURE_FILTER;
  s->gpu_dithering_mode          = SETTINGS_DEFAULT_GPU_DITHERING_MODE;
  s->gpu_line_detect_mode        = SETTINGS_DEFAULT_GPU_LINE_DETECT_MODE;
  s->gpu_downsample_mode         = SETTINGS_DEFAULT_GPU_DOWNSAMPLE_MODE;
  s->gpu_downsample_scale        = 1;
  s->gpu_wireframe_mode          = SETTINGS_DEFAULT_GPU_WIREFRAME_MODE;
  s->display_aspect_ratio        = display_aspect_ratio_auto();
  s->display_deinterlacing_mode  = SETTINGS_DEFAULT_DISPLAY_DEINTERLACING_MODE;
  s->display_crop_mode           = SETTINGS_DEFAULT_DISPLAY_CROP_MODE;
  s->display_fine_crop_mode      = SETTINGS_DEFAULT_DISPLAY_FINE_CROP_MODE;
  s->display_alignment           = SETTINGS_DEFAULT_DISPLAY_ALIGNMENT;
  s->display_rotation            = SETTINGS_DEFAULT_DISPLAY_ROTATION;
  s->display_scaling             = SETTINGS_DEFAULT_DISPLAY_SCALING;
  s->display_scaling_24bit       = SETTINGS_DEFAULT_DISPLAY_SCALING;
  s->display_exclusive_fullscreen_control = SETTINGS_DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN;
  s->display_screenshot_mode     = SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_MODE;
  s->display_screenshot_format   = SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_FORMAT;
  s->display_screenshot_quality  = SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_QUALITY;
  s->gpu_max_queued_frames       = SETTINGS_DEFAULT_GPU_MAX_QUEUED_FRAMES;

  s->gpu_use_thread              = true;
  s->gpu_prefer_gles_context     = SETTINGS_DEFAULT_GPU_PREFER_GLES_CONTEXT;
  s->gpu_scaled_interlacing      = true;

  s->gpu_pgxp_culling            = true;
  s->gpu_pgxp_texture_correction = true;

  s->display_optimal_frame_pacing  = SETTINGS_DEFAULT_OPTIMAL_FRAME_PACING;
  s->display_show_messages         = true;
  s->display_animate_messages      = true;
  s->display_blur_message_backgrounds = true;
  s->display_show_status_indicators = true;

  s->gpu_pgxp_tolerance = -1.0f;
  settings_set_pgxp_depth_clear_threshold(s, SETTINGS_DEFAULT_GPU_PGXP_DEPTH_THRESHOLD);

  s->display_osd_scale  = SETTINGS_DEFAULT_OSD_SCALE;
  s->display_osd_margin = SETTINGS_DEFAULT_OSD_MARGIN;

  /* Defaults match GPUSettings::DEFAULT_DISPLAY_OSD_MESSAGE_DURATIONS:
   * Error / Warning / Info / Quick. */
  s->display_osd_message_duration[0] = 15.0f;
  s->display_osd_message_duration[1] = 10.0f;
  s->display_osd_message_duration[2] = 5.0f;
  s->display_osd_message_duration[3] = 2.5f;
  s->display_osd_message_location    = SETTINGS_DEFAULT_OSD_MESSAGE_LOCATION;

  s->achievements_notification_location    = SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_LOCATION;
  s->achievements_indicator_location       = SETTINGS_DEFAULT_ACHIEVEMENT_INDICATOR_LOCATION;
  s->achievements_challenge_indicator_mode = SETTINGS_DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE;
  s->achievements_notification_scale       = SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_AUTO;
  s->achievements_indicator_scale          = SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_AUTO;

  /* texture replacement defaults */
  s->texture_replacements.dump_replaced_textures = true;
  s->texture_replacements.config.max_hash_cache_entries =
    SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_ENTRIES;
  s->texture_replacements.config.max_hash_cache_vram_usage_mb =
    SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB;
  s->texture_replacements.config.max_replacement_cache_vram_usage_mb =
    SETTINGS_TR_DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_MB;
  s->texture_replacements.config.texture_dump_width_threshold      = 16;
  s->texture_replacements.config.texture_dump_height_threshold     = 16;
  s->texture_replacements.config.vram_write_dump_width_threshold   = 128;
  s->texture_replacements.config.vram_write_dump_height_threshold  = 128;
  s->texture_replacements.config.dump_vram_write_force_alpha_channel = true;
  s->texture_replacements.config.reduce_palette_range              = true;

  /* non-GPU defaults */
  s->cpu_overclock_numerator   = 1;
  s->cpu_overclock_denominator = 1;
  s->dma_max_slice_ticks       = SETTINGS_DEFAULT_DMA_MAX_SLICE_TICKS;
  s->dma_halt_ticks            = SETTINGS_DEFAULT_DMA_HALT_TICKS;
  s->gpu_fifo_size             = SETTINGS_DEFAULT_GPU_FIFO_SIZE;
  s->gpu_max_run_ahead         = SETTINGS_DEFAULT_GPU_MAX_RUN_AHEAD;
  s->region                    = SETTINGS_DEFAULT_CONSOLE_REGION;
  s->gpu_force_video_timing    = SETTINGS_DEFAULT_FORCE_VIDEO_TIMING_MODE;
  s->cpu_execution_mode        = SETTINGS_DEFAULT_CPU_EXECUTION_MODE;
  s->cpu_fastmem_mode          = SETTINGS_DEFAULT_CPU_FASTMEM_MODE;
  s->cpu_recompiler_block_linking = true;
  s->cpu_recompiler_compare_register_files = false;

  s->pio_switch_active             = true;
  s->memory_card_use_playlist_title = true;
  s->bios_patch_fast_boot          = SETTINGS_DEFAULT_FAST_BOOT_VALUE;
  s->apply_compatibility_settings  = true;
  s->apply_game_settings           = true;

  s->rewind_save_slots = 10;
  s->save_state_compression = SETTINGS_DEFAULT_SAVE_STATE_COMPRESSION_MODE;

  s->cdrom_readahead_sectors        = SETTINGS_DEFAULT_CDROM_READAHEAD_SECTORS;
  s->cdrom_mechacon_version         = SETTINGS_DEFAULT_CDROM_MECHACON_VERSION;
  s->cdrom_read_speedup             = 1;
  s->cdrom_seek_speedup             = 1;
  s->cdrom_max_seek_speedup_cycles  = SETTINGS_DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES;
  s->cdrom_max_read_speedup_cycles  = SETTINGS_DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES;

  s->audio_output_volume       = 100;
  s->audio_fast_forward_volume = 100;
  s->inhibit_screensaver       = true;
  s->save_state_on_exit        = true;
  s->create_save_state_backups = SETTINGS_DEFAULT_SAVE_STATE_BACKUPS;
  s->confim_power_off          = true;

  s->achievements_notifications              = true;
  s->achievements_leaderboard_notifications  = true;
  s->achievements_leaderboard_trackers       = true;
  s->achievements_sound_effects              = true;
  s->achievements_progress_indicators        = true;
  s->achievements_prefetch_badges            = SETTINGS_DEFAULT_ACHIEVEMENT_BADGE_PREFETCH;
  s->achievements_notification_duration      = SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME;
  s->achievements_leaderboard_duration       = SETTINGS_DEFAULT_LEADERBOARD_NOTIFICATION_TIME;

  s->emulation_speed                = 1.0f;
  s->fast_forward_speed             = 0.0f;
  s->turbo_speed                    = 0.0f;
  s->rewind_save_frequency          = 10.0f;
  s->display_pre_frame_sleep_buffer = SETTINGS_DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUF;

  /* Ports: pad 0 is controller 1; pads 1..7 inherit DEFAULT_CONTROLLER_2_TYPE
   * (None). */
  s->controller_types[0]  = SETTINGS_DEFAULT_CONTROLLER_1_TYPE;
  s->memory_card_types[0] = SETTINGS_DEFAULT_MEMORY_CARD_1_TYPE;
  for (u32 i = 1; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    s->controller_types[i]  = SETTINGS_DEFAULT_CONTROLLER_2_TYPE;
    s->memory_card_types[i] = SETTINGS_DEFAULT_MEMORY_CARD_2_TYPE;
  }

  s->multitap_mode   = SETTINGS_DEFAULT_MULTITAP_MODE;
  s->pio_device_type = SETTINGS_DEFAULT_PIO_DEVICE_TYPE;
  s->audio_backend   = AUDIO_BACKEND_DEFAULT;

  s->audio_stream_parameters.stretch_mode               = SETTINGS_AUDIO_DEFAULT_STRETCH_MODE;
  s->audio_stream_parameters.output_latency_minimal     = SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MINIMAL;
  s->audio_stream_parameters.output_latency_ms          = SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MS;
  s->audio_stream_parameters.buffer_ms                  = SETTINGS_AUDIO_DEFAULT_BUFFER_MS;
  s->audio_stream_parameters.stretch_sequence_length_ms = SETTINGS_AUDIO_DEFAULT_STRETCH_SEQUENCE_LENGTH;
  s->audio_stream_parameters.stretch_seekwindow_ms      = SETTINGS_AUDIO_DEFAULT_STRETCH_SEEKWINDOW;
  s->audio_stream_parameters.stretch_overlap_ms         = SETTINGS_AUDIO_DEFAULT_STRETCH_OVERLAP;
  s->audio_stream_parameters.stretch_use_quickseek      = SETTINGS_AUDIO_DEFAULT_STRETCH_USE_QUICKSEEK;
  s->audio_stream_parameters.stretch_use_aa_filter      = SETTINGS_AUDIO_DEFAULT_STRETCH_USE_AA_FILTER;

  s->gdb_server_port = SETTINGS_DEFAULT_GDB_SERVER_PORT;
}

void settings_destroy(settings_t* s)
{
  if (!s) return;
  free(s->overlay_image_path);    s->overlay_image_path    = NULL;
  free(s->gpu_adapter);           s->gpu_adapter           = NULL;
  free(s->gpu_sw_use_isa);        s->gpu_sw_use_isa        = NULL;
  free(s->audio_driver);          s->audio_driver          = NULL;
  free(s->audio_output_device);   s->audio_output_device   = NULL;
  free(s->pio_flash_image_path);  s->pio_flash_image_path  = NULL;
  free(s->pcdrv_root);            s->pcdrv_root            = NULL;
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    free(s->memory_card_paths[i]);
    s->memory_card_paths[i] = NULL;
  }
}

/* Linear search through a fixed string table.  Returns the matching index in
 * [0, count) or count on failure.  All enum tables are <16 entries so a hand-
 * unrolled hash isn't worth the complexity. */
static u32 lookup_name(const char* str, const char* const* names, u32 count)
{
  if (!str) return count;
  for (u32 i = 0; i < count; i++) {
    if (strcmp(str, names[i]) == 0)
      return i;
  }
  return count;
}

#define DEFINE_ENUM_NAME_HELPERS(base, enum_t, names_array, display_array, count_value)                                \
  bool settings_parse_##base##_name(const char* str, enum_t* out)                                                      \
  {                                                                                                                    \
    const u32 idx = lookup_name(str, names_array, (u32)(count_value));                                                 \
    if (idx == (u32)(count_value)) return false;                                                                       \
    *out = (enum_t)idx;                                                                                                \
    return true;                                                                                                       \
  }                                                                                                                    \
  const char* settings_get_##base##_name(enum_t v)                                                                     \
  {                                                                                                                    \
    const size_t i = (size_t)v;                                                                                        \
    return (i < (size_t)(count_value)) ? names_array[i] : "";                                                          \
  }                                                                                                                    \
  const char* settings_get_##base##_display_name(enum_t v)                                                             \
  {                                                                                                                    \
    const size_t i = (size_t)v;                                                                                        \
    return (i < (size_t)(count_value)) ? display_array[i] : "";                                                        \
  }

 /*
 *   ParseRendererName -> settings_parse_renderer_name BUT
 *   GetRendererName    -> settings_get_renderer_name
 * which is what the macro above gives us already.  The truly irregular ones
 * (ParseCPUExecutionMode etc.) get a hand-written wrapper after the macro
 * expansions below. */

static const char* const s_log_level_names[] = {
  "None", "Error", "Warning", "Info", "Verbose", "Dev", "Debug", "Trace",
};
static const char* const s_log_level_display_names[] = {
  "None", "Error", "Warning", "Information", "Verbose", "Developer", "Debug", "Trace",
};
DEFINE_ENUM_NAME_HELPERS(log_level, log_level_t,
                         s_log_level_names, s_log_level_display_names, LOG_LEVEL_MAX_COUNT)

static const char* const s_console_region_names[] = {
  "Auto", "NTSC-J", "NTSC-U", "PAL",
};
static const char* const s_console_region_display_names[] = {
  "Auto-Detect", "NTSC-J (Japan)", "NTSC-U/C (US, Canada)", "PAL (Europe, Australia)",
};
DEFINE_ENUM_NAME_HELPERS(console_region, console_region_t,
                         s_console_region_names, s_console_region_display_names, CONSOLE_REGION_COUNT)

static const char* const s_disc_region_names[] = {
  "NTSC-J", "NTSC-U", "PAL", "Other", "Non-PS1",
};
static const char* const s_disc_region_display_names[] = {
  "NTSC-J (Japan)", "NTSC-U/C (US, Canada)", "PAL (Europe, Australia)", "Other", "Non-PS1",
};
DEFINE_ENUM_NAME_HELPERS(disc_region, disc_region_t,
                         s_disc_region_names, s_disc_region_display_names, DISC_REGION_COUNT)

static const char* const s_cpu_execution_mode_names[] = {
  "Interpreter", "CachedInterpreter", "Recompiler",
};
static const char* const s_cpu_execution_mode_display_names[] = {
  "Interpreter (Slowest)", "Cached Interpreter (Faster)", "Recompiler (Fastest)",
};
DEFINE_ENUM_NAME_HELPERS(cpu_execution_mode, cpu_execution_mode_t,
                         s_cpu_execution_mode_names, s_cpu_execution_mode_display_names,
                         CPU_EXECUTION_MODE_COUNT)

static const char* const s_cpu_fastmem_mode_names[] = {
  "Disabled", "MMap", "LUT",
};
static const char* const s_cpu_fastmem_mode_display_names[] = {
  "Disabled (Slowest)", "MMap (Hardware, Fastest, 64-Bit Only)", "LUT (Faster)",
};
DEFINE_ENUM_NAME_HELPERS(cpu_fastmem_mode, cpu_fastmem_mode_t,
                         s_cpu_fastmem_mode_names, s_cpu_fastmem_mode_display_names,
                         CPU_FASTMEM_MODE_COUNT)

static const char* const s_gpu_renderer_names[] = {
  "Automatic", "Software", "OpenGL",
};
static const char* const s_gpu_renderer_display_names[] = {
  "Automatic", "Software", "Hardware (OpenGL)",
};
DEFINE_ENUM_NAME_HELPERS(renderer, gpu_renderer_t,
                         s_gpu_renderer_names, s_gpu_renderer_display_names, GPU_RENDERER_COUNT)

static const char* const s_texture_filter_names[] = {
  "Nearest", "Bilinear", "BilinearBinAlpha", "JINC2", "JINC2BinAlpha",
  "xBR", "xBRBinAlpha", "Scale2x", "Scale3x", "MMPX", "MMPXEnhanced", "MMPXQuality", 
};
static const char* const s_texture_filter_display_names[] = {
  "Nearest-Neighbor", "Bilinear", "Bilinear (No Edge Blending)",
  "JINC2 (Slow)", "JINC2 (Slow, No Edge Blending)",
  "xBR (Very Slow)", "xBR (Very Slow, No Edge Blending)",
  "Scale2x (EPX)", "Scale3x (Slow)",
  "MMPX (Slow)", "MMPX Enhanced (Slow)", "MMPX Quality (Very Slow)",
};
DEFINE_ENUM_NAME_HELPERS(texture_filter, gpu_texture_filter_t,
                         s_texture_filter_names, s_texture_filter_display_names,
                         GPU_TEXTURE_FILTER_COUNT)

static const char* const s_gpu_dithering_mode_names[] = {
  "Unscaled", "UnscaledShaderBlend", "Scaled", "ScaledShaderBlend",
  "TrueColor", "TrueColorFull", 
};
static const char* const s_gpu_dithering_mode_display_names[] = {
  "Unscaled", "Unscaled (Shader Blending)", "Scaled", "Scaled (Shader Blending)",
  "True Color", "True Color (Full)", 
};
DEFINE_ENUM_NAME_HELPERS(gpu_dithering_mode, gpu_dithering_mode_t,
                         s_gpu_dithering_mode_names, s_gpu_dithering_mode_display_names,
                         GPU_DITHERING_MODE_MAX_COUNT)

static const char* const s_line_detect_mode_names[] = {
  "Disabled", "Quads", "BasicTriangles", "AggressiveTriangles",
};
static const char* const s_line_detect_mode_display_names[] = {
  "Disabled", "Quads", "Triangles (Basic)", "Triangles (Aggressive)",
};
DEFINE_ENUM_NAME_HELPERS(line_detect_mode, gpu_line_detect_mode_t,
                         s_line_detect_mode_names, s_line_detect_mode_display_names,
                         GPU_LINE_DETECT_MODE_COUNT)

static const char* const s_downsample_mode_names[] = {
  "Disabled", "Box", "Adaptive",
};
static const char* const s_downsample_mode_display_names[] = {
  "Disabled", "Box (Downsample 3D/Smooth All)", "Adaptive (Preserve 3D/Smooth 2D)",
};
DEFINE_ENUM_NAME_HELPERS(downsample_mode, gpu_downsample_mode_t,
                         s_downsample_mode_names, s_downsample_mode_display_names,
                         GPU_DOWNSAMPLE_MODE_COUNT)

static const char* const s_wireframe_mode_names[] = {
  "Disabled", "OverlayWireframe", "OnlyWireframe",
};
static const char* const s_wireframe_mode_display_names[] = {
  "Disabled", "Overlay Wireframe", "Only Wireframe",
};
DEFINE_ENUM_NAME_HELPERS(gpu_wireframe_mode, gpu_wireframe_mode_t,
                         s_wireframe_mode_names, s_wireframe_mode_display_names,
                         GPU_WIREFRAME_MODE_COUNT)

static const char* const s_gpu_dump_compression_mode_names[] = {
  "Disabled", "ZstLow", "ZstDefault", "ZstHigh", "XZLow", "XZDefault", "XZHigh",
};
static const char* const s_gpu_dump_compression_mode_display_names[] = {
  "Disabled", "Zstandard (Low)", "Zstandard (Default)", "Zstandard (High)",
  "XZ (Low)", "XZ (Default)", "XZ (High)", 
};
DEFINE_ENUM_NAME_HELPERS(gpu_dump_compression_mode, gpu_dump_compression_mode_t,
                          s_gpu_dump_compression_mode_names,
                         s_gpu_dump_compression_mode_display_names, 
                         GPU_DUMP_COMPRESSION_MODE_MAX_COUNT)

static const char* const s_display_deinterlacing_mode_names[] = {
  "Disabled", "Weave", "Blend", "Adaptive", "Progressive",
};
static const char* const s_display_deinterlacing_mode_display_names[] = {
  "Disabled (Flickering)", "Weave (Combing)", "Blend (Blur)",
  "Adaptive (FastMAD)", "Progressive (Optimal)", 
};
DEFINE_ENUM_NAME_HELPERS(display_deinterlacing_mode, display_deinterlacing_mode_t,
                          s_display_deinterlacing_mode_names,
                         s_display_deinterlacing_mode_display_names, 
                         DISPLAY_DEINTERLACING_MODE_COUNT)

static const char* const s_display_crop_mode_names[] = {
  "None", "Overscan", "OverscanUncorrected", "Borders", "BordersUncorrected",
};
static const char* const s_display_crop_mode_display_names[] = {
  "None", "Only Overscan Area", "Only Overscan Area (Aspect Uncorrected)",
  "All Borders", "All Borders (Aspect Uncorrected)",
};
DEFINE_ENUM_NAME_HELPERS(display_crop_mode, display_crop_mode_t,
                         s_display_crop_mode_names, s_display_crop_mode_display_names,
                         DISPLAY_CROP_MODE_MAX_COUNT)

static const char* const s_display_fine_crop_mode_names[] = {
  "None", "VideoResolution", "InternalResolution", "WindowResolution",
};
static const char* const s_display_fine_crop_mode_display_names[] = {
  "None", "Video Resolution", "Internal Resolution", "Window Resolution",
};
DEFINE_ENUM_NAME_HELPERS(display_fine_crop_mode, display_fine_crop_mode_t,
                          s_display_fine_crop_mode_names,
                         s_display_fine_crop_mode_display_names, 
                         DISPLAY_FINE_CROP_MODE_MAX_COUNT)

static const char* const s_display_alignment_names[] = {
  "LeftOrTop", "Center", "RightOrBottom",
};
static const char* const s_display_alignment_display_names[] = {
  "Left / Top", "Center", "Right / Bottom",
};
DEFINE_ENUM_NAME_HELPERS(display_alignment, display_alignment_t,
                         s_display_alignment_names, s_display_alignment_display_names,
                         DISPLAY_ALIGNMENT_COUNT)

static const char* const s_display_rotation_names[] = {
  "Normal", "Rotate90", "Rotate180", "Rotate270",
};
static const char* const s_display_rotation_display_names[] = {
  "No Rotation", "Rotate 90 (Clockwise)", "Rotate 180 (Vertical Flip)", "Rotate 270 (Clockwise)",
};
DEFINE_ENUM_NAME_HELPERS(display_rotation, display_rotation_t,
                         s_display_rotation_names, s_display_rotation_display_names,
                         DISPLAY_ROTATION_COUNT)

static const char* const s_display_scaling_names[] = {
  "Nearest", "NearestInteger", "BilinearSmooth", "BilinearHybrid",
  "BilinearSharp", "BilinearInteger", "Lanczos", 
};
static const char* const s_display_scaling_display_names[] = {
  "Nearest-Neighbor", "Nearest-Neighbor (Integer)",
  "Bilinear (Smooth)", "Bilinear (Hybrid)", "Bilinear (Sharp)",
  "Bilinear (Integer)", "Lanczos (Sharp)", 
};
DEFINE_ENUM_NAME_HELPERS(display_scaling, display_scaling_mode_t,
                         s_display_scaling_names, s_display_scaling_display_names,
                         DISPLAY_SCALING_MODE_COUNT)

static const char* const s_force_video_timing_names[] = {
  "Disabled", "NTSC", "PAL",
};
static const char* const s_force_video_timing_display_names[] = {
  "Auto-Detect", "NTSC (60hz)", "PAL (50hz)",
};
DEFINE_ENUM_NAME_HELPERS(force_video_timing, force_video_timing_mode_t,
                         s_force_video_timing_names, s_force_video_timing_display_names,
                         FORCE_VIDEO_TIMING_MODE_COUNT)

static const char* const s_display_exclusive_fullscreen_names[] = {
  "Automatic", "Disallowed", "Allowed",
};
static const char* const s_display_exclusive_fullscreen_display_names[] = {
  "Automatic", "Disallowed", "Allowed",
};
DEFINE_ENUM_NAME_HELPERS(display_exclusive_fullscreen_control,
                          display_exclusive_fullscreen_control_t,
                         s_display_exclusive_fullscreen_names,
                         s_display_exclusive_fullscreen_display_names, 
                         DISPLAY_EXCLUSIVE_FULLSCREEN_CONTROL_COUNT)

static const char* const s_display_screenshot_mode_names[] = {
  "ScreenResolution", "InternalResolution", "UncorrectedInternalResolution",
};
static const char* const s_display_screenshot_mode_display_names[] = {
  "Screen Resolution", "Internal Resolution", "Internal Resolution (Aspect Uncorrected)",
};
DEFINE_ENUM_NAME_HELPERS(display_screenshot_mode, display_screenshot_mode_t,
                          s_display_screenshot_mode_names,
                         s_display_screenshot_mode_display_names, 
                         DISPLAY_SCREENSHOT_MODE_COUNT)

static const char* const s_display_screenshot_format_names[] = {
  "PNG", "JPEG", "WebP",
};
static const char* const s_display_screenshot_format_display_names[] = {
  "PNG", "JPEG", "WebP",
};
static const char* const s_display_screenshot_format_extensions[] = {
  "png", "jpg", "webp",
};
DEFINE_ENUM_NAME_HELPERS(display_screenshot_format, display_screenshot_format_t,
                          s_display_screenshot_format_names,
                         s_display_screenshot_format_display_names, 
                         DISPLAY_SCREENSHOT_FORMAT_COUNT)

const char* settings_get_display_screenshot_format_extension(display_screenshot_format_t format)
{
  const size_t i = (size_t)format;
  return (i < (size_t)DISPLAY_SCREENSHOT_FORMAT_COUNT)
           ? s_display_screenshot_format_extensions[i] : "";
}

bool settings_get_display_screenshot_format_from_filename(const char* filename,
                                                          display_screenshot_format_t* out)
{
  if (!filename) return false;
  const char* dot = strrchr(filename, '.');
  if (!dot) return false;
  const char* ext = dot + 1;
  for (u32 i = 0; i < DISPLAY_SCREENSHOT_FORMAT_COUNT; i++) {
    if (strcasecmp(ext, s_display_screenshot_format_extensions[i]) == 0) {
      *out = (display_screenshot_format_t)i;
      return true;
    }
  }
  return false;
}

static const char* const s_display_osd_message_type_names[] = {
  "Error", "Warning", "Info", "Quick", "Persistent",
};
const char* settings_get_display_osd_message_type_name(u32 type)
{
  return (type < 5) ? s_display_osd_message_type_names[type] : "";
}

static const char* const s_notification_location_names[] = {
  "TopLeft", "TopCenter", "TopRight",
  "BottomLeft", "BottomCenter", "BottomRight", 
};
static const char* const s_notification_location_display_names[] = {
  "Top Left", "Top Center", "Top Right",
  "Bottom Left", "Bottom Center", "Bottom Right", 
};
DEFINE_ENUM_NAME_HELPERS(notification_location, notification_location_t,
                          s_notification_location_names,
                         s_notification_location_display_names, 
                         NOTIFICATION_LOCATION_MAX_COUNT)

static const char* const s_achievement_challenge_indicator_mode_names[] = {
  "Disabled", "PersistentIcon", "TemporaryIcon", "Notification",
};
static const char* const s_achievement_challenge_indicator_mode_display_names[] = {
  "Disabled", "Show Persistent Icons", "Show Temporary Icons", "Show Notifications",
};
DEFINE_ENUM_NAME_HELPERS(achievement_challenge_indicator_mode,
                          achievement_challenge_indicator_mode_t,
                         s_achievement_challenge_indicator_mode_names,
                         s_achievement_challenge_indicator_mode_display_names, 
                         ACHIEVEMENT_CHALLENGE_INDICATOR_MODE_MAX_COUNT)

static const char* const s_memory_card_type_names[] = {
  "None", "Shared", "PerGame", "PerGameTitle", "PerGameFileTitle", "NonPersistent",
};
static const char* const s_memory_card_type_display_names[] = {
  "No Memory Card", "Shared Between All Games",
  "Separate Card Per Game (Serial)", "Separate Card Per Game (Title)",
  "Separate Card Per Game (File Title)", "Non-Persistent Card (Do Not Save)",
};
DEFINE_ENUM_NAME_HELPERS(memory_card_type, memory_card_type_t,
                         s_memory_card_type_names, s_memory_card_type_display_names,
                         MEMORY_CARD_TYPE_COUNT)

static const char* const s_multitap_mode_names[] = {
  "Disabled", "Port1Only", "Port2Only", "BothPorts",
};
static const char* const s_multitap_mode_display_names[] = {
  "Disabled", "Enable on Port 1 Only", "Enable on Port 2 Only", "Enable on Ports 1 and 2",
};
DEFINE_ENUM_NAME_HELPERS(multitap_mode, multitap_mode_t,
                         s_multitap_mode_names, s_multitap_mode_display_names,
                         MULTITAP_MODE_COUNT)

static const char* const s_mechacon_version_names[] = {
  "VC0A", "VC0B", "VC1A", "VC1B", "VD1", "VC2", "VC1",
  "VC2J", "VC2A", "VC2B", "VC3A", "VC3B", "VC3C", 
};
static const char* const s_mechacon_version_display_names[] = {
  "94/09/19 (VC0A)", "94/11/18 (VC0B)", "95/05/16 (VC1A)", "95/07/24 (VC1B)",
  "95/07/24 (VD1)",  "96/08/15 (VC2)",  "96/08/18 (VC1)",
  "96/09/12 (VC2J)", "97/01/10 (VC2A)", "97/08/14 (VC2B)",
  "98/06/10 (VC3A)", "99/02/01 (VC3B)", "01/03/06 (VC3C)", 
};
DEFINE_ENUM_NAME_HELPERS(cdrom_mech_version, cdrom_mechacon_version_t,
                         s_mechacon_version_names, s_mechacon_version_display_names,
                         CDROM_MECHACON_VERSION_COUNT)

static const char* const s_save_state_compression_mode_names[] = {
  "Uncompressed",
  "DeflateLow", "DeflateDefault", "DeflateHigh",
  "ZstLow",     "ZstDefault",     "ZstHigh",
  "XZLow",      "XZDefault",      "XZHigh", 
};
static const char* const s_save_state_compression_mode_display_names[] = {
  "Uncompressed",
  "Deflate (Low)",   "Deflate (Default)",   "Deflate (High)",
  "Zstandard (Low)", "Zstandard (Default)", "Zstandard (High)",
  "XZ (Low)",        "XZ (Default)",        "XZ (High)", 
};
DEFINE_ENUM_NAME_HELPERS(save_state_compression_mode, save_state_compression_mode_t,
                          s_save_state_compression_mode_names,
                         s_save_state_compression_mode_display_names, 
                         SAVE_STATE_COMPRESSION_MODE_COUNT)

static const char* const s_pio_device_type_names[] = {
  "None", "XplorerCart",
};
static const char* const s_pio_device_type_display_names[] = {
  "None", "Xplorer/Xploder Cartridge",
};
DEFINE_ENUM_NAME_HELPERS(pio_device_type, pio_device_type_t,
                         s_pio_device_type_names, s_pio_device_type_display_names,
                         PIO_DEVICE_TYPE_MAX_COUNT)

#undef DEFINE_ENUM_NAME_HELPERS


/* English noop forms verbatim. */
static const char s_auto_aspect_name[]    = "Auto (Game Native)";
static const char s_stretch_aspect_name[] = "Stretch To Fill";
static const char s_par_1_1_aspect_name[] = "PAR 1:1";

bool settings_parse_display_aspect_ratio(const char* str, display_aspect_ratio_t* out)
{
  if (!str || !str[0]) return false;
  if (strcmp(str, s_auto_aspect_name) == 0)    { *out = display_aspect_ratio_auto();    return true; }
  if (strcmp(str, s_stretch_aspect_name) == 0) { *out = display_aspect_ratio_stretch(); return true; }
  if (strcmp(str, s_par_1_1_aspect_name) == 0) { *out = display_aspect_ratio_par1_1();  return true; }

  /* "n:d" form. */
  const char* colon = strchr(str, ':');
  if (!colon) return false;
  const u32 num_len = (u32)(colon - str);
  const u32 den_len = (u32)strlen(colon + 1);
  s32 num = 0, den = 0;
  if (!string_util_from_chars_s32(str, num_len, 10, &num, NULL)) return false;
  if (!string_util_from_chars_s32(colon + 1, den_len, 10, &den, NULL)) return false;
  if (num <= 0 || den <= 0 || num > 32767 || den > 32767) return false;
  out->numerator   = (s16)num;
  out->denominator = (s16)den;
  return true;
}

void settings_get_display_aspect_ratio_name(display_aspect_ratio_t ar, char* out, size_t out_size)
{
  if (!out || out_size == 0) return;
  display_aspect_ratio_t a_auto    = display_aspect_ratio_auto();
  display_aspect_ratio_t a_stretch = display_aspect_ratio_stretch();
  display_aspect_ratio_t a_par_1_1 = display_aspect_ratio_par1_1();
  if (display_aspect_ratio_equals(&ar, &a_auto))         snprintf(out, out_size, "%s", s_auto_aspect_name);
  else if (display_aspect_ratio_equals(&ar, &a_stretch)) snprintf(out, out_size, "%s", s_stretch_aspect_name);
  else if (display_aspect_ratio_equals(&ar, &a_par_1_1)) snprintf(out, out_size, "%s", s_par_1_1_aspect_name);
  else                                                   snprintf(out, out_size, "%d:%d", ar.numerator, ar.denominator);
}

void settings_get_display_aspect_ratio_display_name(display_aspect_ratio_t ar, char* out, size_t out_size)
{
  /* English-only: identical to the parse name today (translation deferred). */
  settings_get_display_aspect_ratio_name(ar, out, out_size);
}

const display_aspect_ratio_t* settings_get_predefined_display_aspect_ratios(size_t* out_count)
{
  static const display_aspect_ratio_t s_predefined[] = {
    {0, 0},   /* Auto */
    {-1, -1}, /* Stretch */
    {4, 3},
    {16, 9},
    {19, 9},
    {20, 9},
    {21, 9},
    {16, 10},
    {-1, 0},  /* PAR 1:1 */
  };
  if (out_count) *out_count = sizeof(s_predefined) / sizeof(s_predefined[0]);
  return s_predefined;
}

bool settings_is_using_software_renderer(const settings_t* s)
{
  return s->gpu_renderer == GPU_RENDERER_SOFTWARE;
}
bool settings_is_using_true_color(const settings_t* s)
{
  return s->gpu_dithering_mode >= GPU_DITHERING_MODE_TRUE_COLOR;
}
bool settings_is_using_dithering(const settings_t* s)
{
  return s->gpu_dithering_mode < GPU_DITHERING_MODE_TRUE_COLOR;
}
bool settings_is_using_shader_blending(const settings_t* s)
{
  return s->gpu_dithering_mode == GPU_DITHERING_MODE_UNSCALED_SHADER_BLEND ||
         s->gpu_dithering_mode == GPU_DITHERING_MODE_SCALED_SHADER_BLEND;
}
bool settings_is_using_scaled_dithering(const settings_t* s)
{
  return s->gpu_dithering_mode == GPU_DITHERING_MODE_SCALED ||
         s->gpu_dithering_mode == GPU_DITHERING_MODE_SCALED_SHADER_BLEND;
}
bool settings_is_using_integer_display_scaling(const settings_t* s, bool is_24bit)
{
  const display_scaling_mode_t mode = is_24bit ? s->display_scaling_24bit : s->display_scaling;
  return (mode == DISPLAY_SCALING_MODE_NEAREST_INTEGER ||
          mode == DISPLAY_SCALING_MODE_BILINEAR_INTEGER);
}
bool settings_using_pgxp_cpu_mode(const settings_t* s)    { return s->gpu_pgxp_enable && s->gpu_pgxp_cpu; }
bool settings_using_pgxp_depth_buffer(const settings_t* s){ return s->gpu_pgxp_enable && s->gpu_pgxp_depth_buffer; }

bool settings_is_runahead_enabled(const settings_t* s) { return s->runahead_frames > 0; }

u8 settings_get_audio_output_volume(const settings_t* s, bool fast_forwarding)
{
  if (s->audio_output_muted) return 0;
  return fast_forwarding ? s->audio_fast_forward_volume : s->audio_output_volume;
}

bool settings_is_port1_multitap_enabled(const settings_t* s)
{
  return s->multitap_mode == MULTITAP_MODE_PORT1_ONLY ||
         s->multitap_mode == MULTITAP_MODE_BOTH_PORTS;
}
bool settings_is_port2_multitap_enabled(const settings_t* s)
{
  return s->multitap_mode == MULTITAP_MODE_PORT2_ONLY ||
         s->multitap_mode == MULTITAP_MODE_BOTH_PORTS;
}
bool settings_is_multitap_port_enabled(const settings_t* s, u32 port)
{
  return (port == 0) ? settings_is_port1_multitap_enabled(s)
                     : settings_is_port2_multitap_enabled(s);
}

controller_type_t settings_get_default_controller_type(u32 pad)
{
  return (pad == 0) ? SETTINGS_DEFAULT_CONTROLLER_1_TYPE : SETTINGS_DEFAULT_CONTROLLER_2_TYPE;
}
bool settings_is_per_game_memory_card_type(memory_card_type_t type)
{
  return type == MEMORY_CARD_TYPE_PER_GAME ||
         type == MEMORY_CARD_TYPE_PER_GAME_TITLE ||
         type == MEMORY_CARD_TYPE_PER_GAME_FILE_TITLE;
}
bool settings_has_any_per_game_memory_cards(const settings_t* s)
{
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    if (s->memory_card_types[i] == MEMORY_CARD_TYPE_PER_GAME ||
        s->memory_card_types[i] == MEMORY_CARD_TYPE_PER_GAME_TITLE)
      return true;
  }
  return false;
}

static u32 settings_gcd(u32 a, u32 b)
{
  while (b != 0) { const u32 t = b; b = a % b; a = t; }
  return a;
}

void settings_cpu_overclock_percent_to_fraction(u32 percent, u32* numerator, u32* denominator)
{
  const u32 g = settings_gcd(percent, 100u);
  *numerator   = percent / g;
  *denominator = 100u / g;
}

u32 settings_cpu_overclock_fraction_to_percent(u32 numerator, u32 denominator)
{
  return (numerator * 100u) / denominator;
}

void settings_set_cpu_overclock_percent(settings_t* s, u32 percent)
{
  settings_cpu_overclock_percent_to_fraction(percent, &s->cpu_overclock_numerator,
                                             &s->cpu_overclock_denominator);
}

u32 settings_get_cpu_overclock_percent(const settings_t* s)
{
  return settings_cpu_overclock_fraction_to_percent(s->cpu_overclock_numerator,
                                                    s->cpu_overclock_denominator);
}

void settings_update_overclock_active(settings_t* s)
{
  s->cpu_overclock_active = s->cpu_overclock_enable &&
                            (s->cpu_overclock_numerator != 1 || s->cpu_overclock_denominator != 1);
}

static bool settings_str_eq(const char* a, const char* b)
{
  const char* ea = a ? a : "";
  const char* eb = b ? b : "";
  return strcmp(ea, eb) == 0;
}

bool settings_are_gpu_device_settings_changed(const settings_t* a, const settings_t* b)
{
  return !settings_str_eq(a->gpu_adapter, b->gpu_adapter) ||
          a->gpu_use_thread != b->gpu_use_thread ||
         a->gpu_use_debug_device != b->gpu_use_debug_device ||
         a->gpu_use_debug_device_gpu_validation != b->gpu_use_debug_device_gpu_validation ||
         a->gpu_prefer_gles_context != b->gpu_prefer_gles_context ||
         a->gpu_disable_shader_cache != b->gpu_disable_shader_cache ||
         a->gpu_disable_dual_source_blend != b->gpu_disable_dual_source_blend ||
         a->gpu_disable_framebuffer_fetch != b->gpu_disable_framebuffer_fetch ||
         a->gpu_disable_texture_buffers != b->gpu_disable_texture_buffers ||
         a->gpu_disable_texture_copy_to_self != b->gpu_disable_texture_copy_to_self ||
         a->gpu_disable_memory_import != b->gpu_disable_memory_import ||
         a->gpu_disable_raster_order_views != b->gpu_disable_raster_order_views ||
         a->gpu_disable_compute_shaders != b->gpu_disable_compute_shaders ||
         a->gpu_disable_compressed_textures != b->gpu_disable_compressed_textures || 
         a->display_exclusive_fullscreen_control != b->display_exclusive_fullscreen_control;
}

const char* const* settings_get_section_save_order(size_t* out_count)
{
  static const char* const s_order[] = {
    "Patches", "Cheats", "Main", "UI", "GameListTableView", "AutoUpdater",
    "Folders", "GameList", "Cheevos", "Logging", "BIOS", "Console", "CPU",
    "GPU", "Display", "CDROM", "Audio", "MemoryCards", "TextureReplacements",
    "MediaCapture", "InternalPostProcessing", "PostProcessing", "BorderOverlay",
    "InputSources", "ControllerPorts",
    "Pad1", "Pad2", "Pad3", "Pad4", "Pad5", "Pad6", "Pad7", "Pad8",
    "Hotkeys", "PIO", "SIO", "PCDrv", "Debug", "DebugWindows", "Hacks", 
  };
  if (out_count) *out_count = sizeof(s_order) / sizeof(s_order[0]);
  return s_order;
}

void settings_get_controller_settings_section(u32 pad, char* out, size_t out_size)
{
  if (!out || out_size == 0) return;
  snprintf(out, out_size, "Pad%u", pad + 1);
}

void settings_pad_to_port_and_slot(u32 pad, u32* out_port, u32* out_slot)
{
  /* Layout: Pad1 = (port=0, slot=0); Pad2..Pad5 = port=0 multitap slots
   * 1..3 + port=1 base; matches Controller::ConvertPadToPortAndSlot. */
  if (pad == 0)      { *out_port = 0; *out_slot = 0; return; }
  if (pad == 1)      { *out_port = 1; *out_slot = 0; return; }
  if (pad < 5)       { *out_port = 0; *out_slot = pad - 1; return; }
  *out_port = 1; *out_slot = pad - 4;
}

bool settings_pad_is_multitap_slot(u32 pad)
{
  return pad >= 2; /* slots 0..1 are the base ports */
}

/* Parse-with-default: read a string-valued enum from the interface and resolve
 * via a parse helper, falling back to a default on miss. */
#define LOAD_ENUM(target, parse_fn, name_fn, default_value, section, key)                                              \
  do {                                                                                                                 \
    small_string_t _buf; small_string_init(&_buf);                                                                     \
    settings_interface_get_string_value(si, (section), (key), name_fn(default_value), &_buf);                          \
    if (!parse_fn(small_string_c_str(&_buf), &(target)))                                                               \
      (target) = (default_value);                                                                                      \
    small_string_destroy(&_buf);                                                                                       \
  } while (0)

#define LOAD_STR(slot, section, key)  settings_load_string(&(slot), si, (section), (key))

void settings_load_pgxp(settings_t* s, settings_interface_t* si)
{
  s->gpu_pgxp_culling             = settings_interface_get_bool_value(si, "GPU", "PGXPCulling", true);
  s->gpu_pgxp_texture_correction  = settings_interface_get_bool_value(si, "GPU", "PGXPTextureCorrection", true);
  s->gpu_pgxp_color_correction    = settings_interface_get_bool_value(si, "GPU", "PGXPColorCorrection", false);
  s->gpu_pgxp_vertex_cache        = settings_interface_get_bool_value(si, "GPU", "PGXPVertexCache", false);
  s->gpu_pgxp_cpu                 = settings_interface_get_bool_value(si, "GPU", "PGXPCPU", false);
  s->gpu_pgxp_preserve_proj_fp    = settings_interface_get_bool_value(si, "GPU", "PGXPPreserveProjFP", false);
  s->gpu_pgxp_tolerance           = settings_interface_get_float_value(si, "GPU", "PGXPTolerance", -1.0f);
  s->gpu_pgxp_depth_buffer        = settings_interface_get_bool_value(si, "GPU", "PGXPDepthBuffer", false);
  s->gpu_pgxp_disable_2d          = settings_interface_get_bool_value(si, "GPU", "PGXPDisableOn2DPolygons", false);
  s->gpu_pgxp_transparent_depth   = settings_interface_get_bool_value(si, "GPU", "PGXPTransparentDepthTest", false);
  settings_set_pgxp_depth_clear_threshold(
    s, settings_interface_get_float_value(si, "GPU", "PGXPDepthThreshold",
                                          SETTINGS_DEFAULT_GPU_PGXP_DEPTH_THRESHOLD));
}

static const char* const k_audio_stretch_mode_names[] = {
  "Off", "Resample", "TimeStretch",
};

static const char* audio_stretch_mode_name(audio_stretch_mode_t m)
{
  return ((u32)m < AUDIO_STRETCH_MODE_COUNT) ? k_audio_stretch_mode_names[m] : "TimeStretch";
}

static bool audio_stretch_mode_parse(const char* s, audio_stretch_mode_t* out)
{
  if (!s) return false;
  for (u32 i = 0; i < AUDIO_STRETCH_MODE_COUNT; i++) {
    if (strcmp(s, k_audio_stretch_mode_names[i]) == 0) {
      *out = (audio_stretch_mode_t)i;
      return true;
    }
  }
  return false;
}

static void audio_params_load(settings_audio_stream_parameters_t* p,
                              settings_interface_t* si, const char* section)
{
  small_string_t buf;
  small_string_init(&buf);

  settings_interface_get_string_value(si, section, "StretchMode",
                                      audio_stretch_mode_name(SETTINGS_AUDIO_DEFAULT_STRETCH_MODE),
                                      &buf);
  if (!audio_stretch_mode_parse(small_string_c_str(&buf), &p->stretch_mode))
    p->stretch_mode = SETTINGS_AUDIO_DEFAULT_STRETCH_MODE;
  small_string_destroy(&buf);

  p->output_latency_minimal      = settings_interface_get_bool_value(si, section,
                                     "OutputLatencyMinimal", SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MINIMAL);
  p->output_latency_ms           = settings_interface_get_saturated_u16(si, section,
                                     "OutputLatencyMS", SETTINGS_AUDIO_DEFAULT_OUTPUT_LATENCY_MS);
  p->buffer_ms                   = settings_interface_get_saturated_u16(si, section,
                                     "BufferMS", SETTINGS_AUDIO_DEFAULT_BUFFER_MS);
  p->stretch_sequence_length_ms  = settings_interface_get_saturated_u16(si, section,
                                     "StretchSequenceLengthMS", SETTINGS_AUDIO_DEFAULT_STRETCH_SEQUENCE_LENGTH);
  p->stretch_seekwindow_ms       = settings_interface_get_saturated_u16(si, section,
                                     "StretchSeekWindowMS", SETTINGS_AUDIO_DEFAULT_STRETCH_SEEKWINDOW);
  p->stretch_overlap_ms          = settings_interface_get_saturated_u16(si, section,
                                     "StretchOverlapMS", SETTINGS_AUDIO_DEFAULT_STRETCH_OVERLAP);
  p->stretch_use_quickseek       = settings_interface_get_bool_value(si, section,
                                     "StretchUseQuickseek", SETTINGS_AUDIO_DEFAULT_STRETCH_USE_QUICKSEEK);
  p->stretch_use_aa_filter       = settings_interface_get_bool_value(si, section,
                                     "StretchUseAAFilter", SETTINGS_AUDIO_DEFAULT_STRETCH_USE_AA_FILTER);
}

static void audio_params_save(const settings_audio_stream_parameters_t* p,
                              settings_interface_t* si, const char* section)
{
  settings_interface_set_string_value(si, section, "StretchMode",
                                      audio_stretch_mode_name(p->stretch_mode));
  settings_interface_set_bool_value(si, section, "OutputLatencyMinimal", p->output_latency_minimal);
  settings_interface_set_uint_value(si, section, "OutputLatencyMS", p->output_latency_ms);
  settings_interface_set_uint_value(si, section, "BufferMS", p->buffer_ms);
  settings_interface_set_uint_value(si, section, "StretchSequenceLengthMS", p->stretch_sequence_length_ms);
  settings_interface_set_uint_value(si, section, "StretchSeekWindowMS", p->stretch_seekwindow_ms);
  settings_interface_set_uint_value(si, section, "StretchOverlapMS", p->stretch_overlap_ms);
  settings_interface_set_bool_value(si, section, "StretchUseQuickseek", p->stretch_use_quickseek);
  settings_interface_set_bool_value(si, section, "StretchUseAAFilter", p->stretch_use_aa_filter);
}

/* Saturate u32 to u16 (equivalent to std::min<u16,u32>(x, USHRT_MAX)). */
static u16 sat_u32_to_u16(u32 v) { return v > 0xFFFFu ? (u16)0xFFFFu : (u16)v; }
static u8  sat_u32_to_u8(u32 v)  { return v > 0xFFu   ? (u8)0xFFu   : (u8)v; }
static u32 max_u32(u32 a, u32 b) { return a > b ? a : b; }
static u32 clamp_u32(u32 v, u32 lo, u32 hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void settings_load(settings_t* s, settings_interface_t* si, settings_interface_t* controller_si)
{
  /* Console */
  LOAD_ENUM(s->region, settings_parse_console_region_name, settings_get_console_region_name,
            SETTINGS_DEFAULT_CONSOLE_REGION, "Console", "Region");
  s->cpu_enable_8mb_ram = settings_interface_get_bool_value(si, "Console", "Enable8MBRAM", false);

  /* Main */
  s->emulation_speed                  = settings_interface_get_float_value(si, "Main", "EmulationSpeed", 1.0f);
  s->fast_forward_speed               = settings_interface_get_float_value(si, "Main", "FastForwardSpeed", 0.0f);
  s->turbo_speed                      = settings_interface_get_float_value(si, "Main", "TurboSpeed", 0.0f);
  s->sync_to_host_refresh_rate        = settings_interface_get_bool_value(si, "Main", "SyncToHostRefreshRate", false);
  s->inhibit_screensaver              = settings_interface_get_bool_value(si, "Main", "InhibitScreensaver", true);
  s->pause_on_focus_loss              = settings_interface_get_bool_value(si, "Main", "PauseOnFocusLoss", false);
  s->pause_on_controller_disconnection= settings_interface_get_bool_value(si, "Main", "PauseOnControllerDisconnection", false);
  s->disable_background_input         = settings_interface_get_bool_value(si, "Main", "DisableBackgroundInput", false);
  s->save_state_on_exit               = settings_interface_get_bool_value(si, "Main", "SaveStateOnExit", true);
  s->create_save_state_backups        = settings_interface_get_bool_value(si, "Main", "CreateSaveStateBackups",
                                                                          SETTINGS_DEFAULT_SAVE_STATE_BACKUPS);
  s->confim_power_off                 = settings_interface_get_bool_value(si, "Main", "ConfirmPowerOff", true);
  s->load_devices_from_save_states    = settings_interface_get_bool_value(si, "Main", "LoadDevicesFromSaveStates", false);
  s->apply_compatibility_settings     = settings_interface_get_bool_value(si, "Main", "ApplyCompatibilitySettings", true);
  s->apply_game_settings              = settings_interface_get_bool_value(si, "Main", "ApplyGameSettings", true);
  s->disable_all_enhancements         = settings_interface_get_bool_value(si, "Main", "DisableAllEnhancements", false);
  s->enable_discord_presence          = settings_interface_get_bool_value(si, "Main", "EnableDiscordPresence", false);
  s->rewind_enable                    = settings_interface_get_bool_value(si, "Main", "RewindEnable", false);
  s->rewind_save_frequency            = settings_interface_get_float_value(si, "Main", "RewindFrequency", 10.0f);
  s->rewind_save_slots                = sat_u32_to_u16(settings_interface_get_uint_value(si, "Main", "RewindSaveSlots", 10u));
  s->runahead_frames                  = sat_u32_to_u8(settings_interface_get_uint_value(si, "Main", "RunaheadFrameCount", 0u));
  s->runahead_for_analog_input        = settings_interface_get_bool_value(si, "Main", "RunaheadForAnalogInput", false);

  /* CPU */
  LOAD_ENUM(s->cpu_execution_mode, settings_parse_cpu_execution_mode_name,
            settings_get_cpu_execution_mode_name,
            SETTINGS_DEFAULT_CPU_EXECUTION_MODE, "CPU", "ExecutionMode");
  s->cpu_overclock_numerator      = max_u32(settings_interface_get_uint_value(si, "CPU", "OverclockNumerator", 1u), 1u);
  s->cpu_overclock_denominator    = max_u32(settings_interface_get_uint_value(si, "CPU", "OverclockDenominator", 1u), 1u);
  s->cpu_overclock_enable         = settings_interface_get_bool_value(si, "CPU", "OverclockEnable", false);
  settings_update_overclock_active(s);
  s->cpu_recompiler_memory_exceptions = settings_interface_get_bool_value(si, "CPU", "RecompilerMemoryExceptions", false);
  s->cpu_recompiler_block_linking     = settings_interface_get_bool_value(si, "CPU", "RecompilerBlockLinking", true);
  s->cpu_recompiler_icache            = settings_interface_get_bool_value(si, "CPU", "RecompilerICache", false);
  s->cpu_recompiler_compare_register_files = settings_interface_get_bool_value(si, "CPU", "RecompilerCompareRegisterFiles", false);
  LOAD_ENUM(s->cpu_fastmem_mode, settings_parse_cpu_fastmem_mode_name, settings_get_cpu_fastmem_mode_name,
            SETTINGS_DEFAULT_CPU_FASTMEM_MODE, "CPU", "FastmemMode");

  /* GPU */
  LOAD_ENUM(s->gpu_renderer, settings_parse_renderer_name, settings_get_renderer_name,
            SETTINGS_DEFAULT_GPU_RENDERER, "GPU", "Renderer");
  LOAD_STR(s->gpu_adapter, "GPU", "Adapter");
  LOAD_STR(s->gpu_sw_use_isa, "GPU", "UseISA");
  s->gpu_resolution_scale         = sat_u32_to_u8(settings_interface_get_uint_value(si, "GPU", "ResolutionScale", 1u));
  s->gpu_automatic_resolution_scale = (s->gpu_resolution_scale == 0);
  s->gpu_multisamples             = sat_u32_to_u8(settings_interface_get_uint_value(si, "GPU", "Multisamples", 1u));
  s->gpu_use_debug_device         = settings_interface_get_bool_value(si, "GPU", "UseDebugDevice", false);
  s->gpu_use_debug_device_gpu_validation = settings_interface_get_bool_value(si, "GPU", "UseGPUBasedValidation", false);
  s->gpu_prefer_gles_context      = settings_interface_get_bool_value(si, "GPU", "PreferGLESContext",
                                                                      SETTINGS_DEFAULT_GPU_PREFER_GLES_CONTEXT);
  s->gpu_disable_shader_cache     = settings_interface_get_bool_value(si, "GPU", "DisableShaderCache", false);
  s->gpu_disable_dual_source_blend= settings_interface_get_bool_value(si, "GPU", "DisableDualSourceBlend", false);
  s->gpu_disable_framebuffer_fetch= settings_interface_get_bool_value(si, "GPU", "DisableFramebufferFetch", false);
  s->gpu_disable_texture_buffers  = settings_interface_get_bool_value(si, "GPU", "DisableTextureBuffers", false);
  s->gpu_disable_texture_copy_to_self = settings_interface_get_bool_value(si, "GPU", "DisableTextureCopyToSelf", false);
  s->gpu_disable_memory_import    = settings_interface_get_bool_value(si, "GPU", "DisableMemoryImport", false);
  s->gpu_disable_raster_order_views = settings_interface_get_bool_value(si, "GPU", "DisableRasterOrderViews", false);
  s->gpu_disable_compute_shaders  = settings_interface_get_bool_value(si, "GPU", "DisableComputeShaders", false);
  s->gpu_disable_compressed_textures = settings_interface_get_bool_value(si, "GPU", "DisableCompressedTextures", false);
  s->gpu_per_sample_shading       = settings_interface_get_bool_value(si, "GPU", "PerSampleShading", false);
  s->gpu_use_thread               = settings_interface_get_bool_value(si, "GPU", "UseThread", true);
  s->gpu_max_queued_frames        = sat_u32_to_u8(settings_interface_get_uint_value(si, "GPU", "MaxQueuedFrames",
                                                                                    SETTINGS_DEFAULT_GPU_MAX_QUEUED_FRAMES));
  /* Default true in c-ps1: SW dual-write to g_vram is always populated by the
   * HW backend's hw_draw_polygon SW path, so reading back from g_vram for
   * display extract is correct and free.  The GL VBO fast path's draws don't
   * yet land visible content in vram_texture (deferred; see PLAN.md), so
   * forcing the SW readback path is what makes OpenGL renderer show pixels
   * instead of black.  Upstream duckstation toggles this per-game via
   * GameDB; we flip the default. */
  /* Default: true.  Mesa Intel iGPUs serialize `glGetTextureSubImage` through
   * the host CPU and stall the audio thread (heavy underruns).  The SW row
   * converter reads `g_vram` (filled by SW dual-write) at full framerate.
   * GameDB / settings.ini can flip this off per-game on capable GPUs. */
  s->gpu_use_software_renderer_for_readbacks = settings_interface_get_bool_value(si, "GPU", "UseSoftwareRendererForReadbacks", true);
  s->gpu_use_software_renderer_for_memory_states = settings_interface_get_bool_value(si, "GPU", "UseSoftwareRendererForMemoryStates", false);
  s->gpu_scaled_interlacing       = settings_interface_get_bool_value(si, "GPU", "ScaledInterlacing", true);
  s->gpu_force_round_texcoords    = settings_interface_get_bool_value(si, "GPU", "ForceRoundTextureCoordinates", false);

  LOAD_ENUM(s->gpu_texture_filter, settings_parse_texture_filter_name, settings_get_texture_filter_name,
            SETTINGS_DEFAULT_GPU_TEXTURE_FILTER, "GPU", "TextureFilter");
  LOAD_ENUM(s->gpu_sprite_texture_filter, settings_parse_texture_filter_name, settings_get_texture_filter_name,
            SETTINGS_DEFAULT_GPU_TEXTURE_FILTER, "GPU", "SpriteTextureFilter");
  LOAD_ENUM(s->gpu_dithering_mode, settings_parse_gpu_dithering_mode_name, settings_get_gpu_dithering_mode_name,
            SETTINGS_DEFAULT_GPU_DITHERING_MODE, "GPU", "DitheringMode");
  LOAD_ENUM(s->gpu_line_detect_mode, settings_parse_line_detect_mode_name, settings_get_line_detect_mode_name,
            SETTINGS_DEFAULT_GPU_LINE_DETECT_MODE, "GPU", "LineDetectMode");
  LOAD_ENUM(s->gpu_downsample_mode, settings_parse_downsample_mode_name, settings_get_downsample_mode_name,
            SETTINGS_DEFAULT_GPU_DOWNSAMPLE_MODE, "GPU", "DownsampleMode");
  s->gpu_downsample_scale = sat_u32_to_u8(settings_interface_get_uint_value(si, "GPU", "DownsampleScale", 1u));
  LOAD_ENUM(s->gpu_wireframe_mode, settings_parse_gpu_wireframe_mode_name, settings_get_gpu_wireframe_mode_name,
            SETTINGS_DEFAULT_GPU_WIREFRAME_MODE, "GPU", "WireframeMode");
  LOAD_ENUM(s->gpu_force_video_timing, settings_parse_force_video_timing_name,
            settings_get_force_video_timing_name,
            SETTINGS_DEFAULT_FORCE_VIDEO_TIMING_MODE, "GPU", "ForceVideoTiming");

  s->gpu_widescreen_hack          = settings_interface_get_bool_value(si, "GPU", "WidescreenHack", false);
  s->gpu_widescreen_rendering     = s->gpu_widescreen_hack;
  s->gpu_modulation_crop          = settings_interface_get_bool_value(si, "GPU", "EnableModulationCrop", false);
  s->gpu_texture_cache            = settings_interface_get_bool_value(si, "GPU", "EnableTextureCache", false);
  s->display_24bit_chroma_smoothing = settings_interface_get_bool_value(si, "GPU", "ChromaSmoothing24Bit", false);
  s->gpu_pgxp_enable              = settings_interface_get_bool_value(si, "GPU", "PGXPEnable", false);
  settings_load_pgxp(s, si);

  s->gpu_show_vram                = settings_interface_get_bool_value(si, "Debug", "ShowVRAM", false);
  s->gpu_dump_cpu_to_vram_copies  = settings_interface_get_bool_value(si, "Debug", "DumpCPUToVRAMCopies", false);
  s->gpu_dump_vram_to_cpu_copies  = settings_interface_get_bool_value(si, "Debug", "DumpVRAMToCPUCopies", false);
  s->gpu_dump_fast_replay_mode    = settings_interface_get_bool_value(si, "GPU", "DumpFastReplayMode", false);

  /* Display */
  LOAD_ENUM(s->display_deinterlacing_mode, settings_parse_display_deinterlacing_mode_name,
            settings_get_display_deinterlacing_mode_name,
            SETTINGS_DEFAULT_DISPLAY_DEINTERLACING_MODE, "GPU", "DeinterlacingMode");
  LOAD_ENUM(s->display_crop_mode, settings_parse_display_crop_mode_name, settings_get_display_crop_mode_name,
            SETTINGS_DEFAULT_DISPLAY_CROP_MODE, "Display", "CropMode");

  /* aspect ratio: own parser */
  {
    small_string_t buf;
    small_string_init(&buf);
    settings_interface_get_string_value(si, "Display", "AspectRatio", "", &buf);
    if (!settings_parse_display_aspect_ratio(small_string_c_str(&buf), &s->display_aspect_ratio))
      s->display_aspect_ratio = display_aspect_ratio_auto();
    small_string_destroy(&buf);
  }

  {
    small_string_t buf;
    small_string_init(&buf);
    settings_interface_get_string_value(si, "Display", "FineCropMode", "", &buf);
    if (!settings_parse_display_fine_crop_mode_name(small_string_c_str(&buf), &s->display_fine_crop_mode))
      s->display_fine_crop_mode = SETTINGS_DEFAULT_DISPLAY_FINE_CROP_MODE;
    small_string_destroy(&buf);
  }
  s->display_fine_crop_amount[0]  = settings_interface_get_saturated_s16(si, "Display", "FineCropLeft", 0);
  s->display_fine_crop_amount[1]  = settings_interface_get_saturated_s16(si, "Display", "FineCropTop", 0);
  s->display_fine_crop_amount[2]  = settings_interface_get_saturated_s16(si, "Display", "FineCropRight", 0);
  s->display_fine_crop_amount[3]  = settings_interface_get_saturated_s16(si, "Display", "FineCropBottom", 0);
  LOAD_ENUM(s->display_alignment, settings_parse_display_alignment_name, settings_get_display_alignment_name,
            SETTINGS_DEFAULT_DISPLAY_ALIGNMENT, "Display", "Alignment");
  LOAD_ENUM(s->display_rotation, settings_parse_display_rotation_name, settings_get_display_rotation_name,
            SETTINGS_DEFAULT_DISPLAY_ROTATION, "Display", "Rotation");
  LOAD_ENUM(s->display_scaling, settings_parse_display_scaling_name, settings_get_display_scaling_name,
            SETTINGS_DEFAULT_DISPLAY_SCALING, "Display", "Scaling");
  LOAD_ENUM(s->display_scaling_24bit, settings_parse_display_scaling_name, settings_get_display_scaling_name,
            SETTINGS_DEFAULT_DISPLAY_SCALING, "Display", "Scaling24Bit");
  LOAD_ENUM(s->display_exclusive_fullscreen_control,
             settings_parse_display_exclusive_fullscreen_control_name,
            settings_get_display_exclusive_fullscreen_control_name, 
            SETTINGS_DEFAULT_DISPLAY_EXCLUSIVE_FULLSCREEN, "Display", "ExclusiveFullscreenControl");
  LOAD_ENUM(s->display_screenshot_mode, settings_parse_display_screenshot_mode_name,
            settings_get_display_screenshot_mode_name,
            SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_MODE, "Display", "ScreenshotMode");
  LOAD_ENUM(s->display_screenshot_format, settings_parse_display_screenshot_format_name,
            settings_get_display_screenshot_format_name,
            SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_FORMAT, "Display", "ScreenshotFormat");
  s->display_screenshot_quality = (u8)clamp_u32(
    settings_interface_get_uint_value(si, "Display", "ScreenshotQuality", SETTINGS_DEFAULT_DISPLAY_SCREENSHOT_QUALITY),
    1u, 100u);
  s->display_optimal_frame_pacing      = settings_interface_get_bool_value(si, "Display", "OptimalFramePacing",
                                                                           SETTINGS_DEFAULT_OPTIMAL_FRAME_PACING);
  s->display_pre_frame_sleep           = settings_interface_get_bool_value(si, "Display", "PreFrameSleep", false);
  s->display_pre_frame_sleep_buffer    = settings_interface_get_float_value(si, "Display", "PreFrameSleepBuffer",
                                                                            SETTINGS_DEFAULT_DISPLAY_PRE_FRAME_SLEEP_BUF);
  s->display_skip_presenting_duplicate_frames = settings_interface_get_bool_value(si, "Display",
                                                                                  "SkipPresentingDuplicateFrames", false);
  s->display_vsync                     = settings_interface_get_bool_value(si, "Display", "VSync", false);
  s->display_disable_mailbox_presentation = settings_interface_get_bool_value(si, "Display", "DisableMailboxPresentation", false);
  s->display_force_4_3_for_24bit       = settings_interface_get_bool_value(si, "Display", "Force4_3For24Bit", false);
  s->display_active_start_offset       = (s16)settings_interface_get_int_value(si, "Display", "ActiveStartOffset", 0);
  s->display_active_end_offset         = (s16)settings_interface_get_int_value(si, "Display", "ActiveEndOffset", 0);
  s->display_line_start_offset         = (s8)settings_interface_get_int_value(si, "Display", "LineStartOffset", 0);
  s->display_line_end_offset           = (s8)settings_interface_get_int_value(si, "Display", "LineEndOffset", 0);
  s->display_show_messages             = settings_interface_get_bool_value(si, "Display", "ShowOSDMessages", true);
  s->display_animate_messages          = settings_interface_get_bool_value(si, "Display", "AnimateOSDMessages", true);
  s->display_blur_message_backgrounds  = settings_interface_get_bool_value(si, "Display", "BlurOSDMessageBackgrounds", true);
  s->display_show_fps                  = settings_interface_get_bool_value(si, "Display", "ShowFPS", false);
  s->display_show_speed                = settings_interface_get_bool_value(si, "Display", "ShowSpeed", false);
  s->display_show_gpu_stats            = settings_interface_get_bool_value(si, "Display", "ShowGPUStatistics", false);
  s->display_show_resolution           = settings_interface_get_bool_value(si, "Display", "ShowResolution", false);
  s->display_show_latency_stats        = settings_interface_get_bool_value(si, "Display", "ShowLatencyStatistics", false);
  s->display_show_cpu_usage            = settings_interface_get_bool_value(si, "Display", "ShowCPU", false);
  s->display_show_gpu_usage            = settings_interface_get_bool_value(si, "Display", "ShowGPU", false);
  s->display_show_frame_times          = settings_interface_get_bool_value(si, "Display", "ShowFrameTimes", false);
  s->display_show_status_indicators    = settings_interface_get_bool_value(si, "Display", "ShowStatusIndicators", true);
  s->display_show_inputs               = settings_interface_get_bool_value(si, "Display", "ShowInputs", false);
  s->display_show_enhancements         = settings_interface_get_bool_value(si, "Display", "ShowEnhancements", false);
  s->display_auto_resize_window        = settings_interface_get_bool_value(si, "Display", "AutoResizeWindow", false);
  s->display_osd_scale                 = settings_interface_get_float_value(si, "Display", "OSDScale", SETTINGS_DEFAULT_OSD_SCALE);
  {
    const float v = settings_interface_get_float_value(si, "Display", "OSDMargin", SETTINGS_DEFAULT_OSD_MARGIN);
    s->display_osd_margin = v < 0.0f ? 0.0f : v;
  }

  /* OSD message duration: per-type key e.g. "OSDErrorDuration". */
  for (u32 i = 0; i < 4u; i++) {
    char key[32];
    snprintf(key, sizeof(key), "OSD%sDuration", settings_get_display_osd_message_type_name(i));
    s->display_osd_message_duration[i] = settings_interface_get_float_value(si, "Display", key,
                                                                            s->display_osd_message_duration[i]);
  }
  {
    small_string_t buf; small_string_init(&buf);
    settings_interface_get_string_value(si, "Display", "OSDMessageLocation", "", &buf);
    if (!settings_parse_notification_location_name(small_string_c_str(&buf), &s->display_osd_message_location))
      s->display_osd_message_location = SETTINGS_DEFAULT_OSD_MESSAGE_LOCATION;
    small_string_destroy(&buf);
  }

  LOAD_ENUM(s->save_state_compression, settings_parse_save_state_compression_mode_name,
            settings_get_save_state_compression_mode_name,
            SETTINGS_DEFAULT_SAVE_STATE_COMPRESSION_MODE, "Main", "SaveStateCompression");

  /* CDROM */
  s->cdrom_readahead_sectors = (u8)settings_interface_get_int_value(si, "CDROM", "ReadaheadSectors",
                                                                    SETTINGS_DEFAULT_CDROM_READAHEAD_SECTORS);
  LOAD_ENUM(s->cdrom_mechacon_version, settings_parse_cdrom_mech_version_name,
            settings_get_cdrom_mech_version_name,
            SETTINGS_DEFAULT_CDROM_MECHACON_VERSION, "CDROM", "MechaconVersion");
  s->cdrom_region_check          = settings_interface_get_bool_value(si, "CDROM", "RegionCheck", false);
  s->cdrom_subq_skew             = settings_interface_get_bool_value(si, "CDROM", "SubQSkew", false);
  s->cdrom_load_image_to_ram     = settings_interface_get_bool_value(si, "CDROM", "LoadImageToRAM", false);
  s->cdrom_load_image_patches    = settings_interface_get_bool_value(si, "CDROM", "LoadImagePatches", false);
  s->cdrom_ignore_host_subcode   = settings_interface_get_bool_value(si, "CDROM", "IgnoreHostSubcode", false);
  s->cdrom_mute_cd_audio         = settings_interface_get_bool_value(si, "CDROM", "MuteCDAudio", false);
  s->cdrom_auto_disc_change      = settings_interface_get_bool_value(si, "CDROM", "AutoDiscChange", false);
  s->cdrom_read_speedup          = settings_interface_get_saturated_u8(si, "CDROM", "ReadSpeedup", 1);
  s->cdrom_seek_speedup          = settings_interface_get_saturated_u8(si, "CDROM", "SeekSpeedup", 1);
  s->cdrom_max_seek_speedup_cycles = max_u32(settings_interface_get_uint_value(si, "CDROM", "MaxSeekSpeedupCycles",
                                                                               SETTINGS_DEFAULT_CDROM_MAX_SEEK_SPEEDUP_CYCLES), 1u);
  s->cdrom_max_read_speedup_cycles = max_u32(settings_interface_get_uint_value(si, "CDROM", "MaxReadSpeedupCycles",
                                                                               SETTINGS_DEFAULT_CDROM_MAX_READ_SPEEDUP_CYCLES), 1u);
  s->mdec_disable_cdrom_speedup  = settings_interface_get_bool_value(si, "CDROM", "DisableSpeedupOnMDEC", false);

  /* Audio */
  {
    small_string_t buf; small_string_init(&buf);
    settings_interface_get_string_value(si, "Audio", "Backend",
                                        audio_stream_get_backend_name(AUDIO_BACKEND_DEFAULT), &buf);
    if (!audio_stream_parse_backend_name(small_string_c_str(&buf), &s->audio_backend))
      s->audio_backend = AUDIO_BACKEND_DEFAULT;
    small_string_destroy(&buf);
  }
  LOAD_STR(s->audio_driver, "Audio", "Driver");
  LOAD_STR(s->audio_output_device, "Audio", "OutputDevice");
  audio_params_load(&s->audio_stream_parameters, si, "Audio");
  s->audio_output_volume       = settings_interface_get_saturated_u8(si, "Audio", "OutputVolume", 100);
  s->audio_fast_forward_volume = settings_interface_get_saturated_u8(si, "Audio", "FastForwardVolume", 100);
  s->audio_output_muted        = settings_interface_get_bool_value(si, "Audio", "OutputMuted", false);

  /* Hacks */
  s->mdec_use_old_routines     = settings_interface_get_bool_value(si, "Hacks", "UseOldMDECRoutines", false);
  s->export_shared_memory      = settings_interface_get_bool_value(si, "Hacks", "ExportSharedMemory", false);
  s->dma_max_slice_ticks       = settings_interface_get_int_value(si, "Hacks", "DMAMaxSliceTicks",
                                                                  SETTINGS_DEFAULT_DMA_MAX_SLICE_TICKS);
  s->dma_halt_ticks            = settings_interface_get_int_value(si, "Hacks", "DMAHaltTicks",
                                                                  SETTINGS_DEFAULT_DMA_HALT_TICKS);
  s->gpu_fifo_size             = settings_interface_get_uint_value(si, "Hacks", "GPUFIFOSize",
                                                                   SETTINGS_DEFAULT_GPU_FIFO_SIZE);
  s->gpu_max_run_ahead         = settings_interface_get_int_value(si, "Hacks", "GPUMaxRunAhead",
                                                                  SETTINGS_DEFAULT_GPU_MAX_RUN_AHEAD);

  /* BIOS */
  s->bios_tty_logging        = settings_interface_get_bool_value(si, "BIOS", "TTYLogging", false);
  s->bios_patch_fast_boot    = settings_interface_get_bool_value(si, "BIOS", "PatchFastBoot",
                                                                 SETTINGS_DEFAULT_FAST_BOOT_VALUE);
  s->bios_fast_forward_boot  = settings_interface_get_bool_value(si, "BIOS", "FastForwardBoot", false);

  LOAD_ENUM(s->multitap_mode, settings_parse_multitap_mode_name, settings_get_multitap_mode_name,
            SETTINGS_DEFAULT_MULTITAP_MODE, "ControllerPorts", "MultitapMode");
  {
    const bool mtap_enabled[2] = {
      settings_is_port1_multitap_enabled(s),
      settings_is_port2_multitap_enabled(s),
    };
    for (u32 pad = 0; pad < NUM_CONTROLLER_AND_CARD_PORTS; pad++) {
      u32 port, slot;
      settings_pad_to_port_and_slot(pad, &port, &slot);
      if (settings_pad_is_multitap_slot(pad) && !mtap_enabled[port]) {
        s->controller_types[pad] = CONTROLLER_TYPE_NONE;
        continue;
      }
      const controller_type_t default_type = (pad == 0) ? SETTINGS_DEFAULT_CONTROLLER_1_TYPE
                                                         : SETTINGS_DEFAULT_CONTROLLER_2_TYPE;
      char section[16];
      settings_get_controller_settings_section(pad, section, sizeof(section));

      small_string_t buf; small_string_init(&buf);
      /* Map type names ("AnalogController", "DigitalController", ...) only by
       * checking the type-token directly.  Replace with the real parse once
       * Controller is ported. */
      settings_interface_get_string_value(controller_si, section, "Type", "", &buf);
      const char* type_str = small_string_c_str(&buf);
      controller_type_t parsed = default_type;
      static const char* const k_controller_type_names[] = {
        "None",                /* CONTROLLER_TYPE_NONE */
        "DigitalController",
        "AnalogController",
        "AnalogJoystick",
        "GunCon",
        "PlayStationMouse",
        "NeGcon",
        "NeGconRumble",
        "Justifier",
        "PopnController",
        "DDGoController",
        "Jogcon", 
      };
      bool matched = false;
      for (u32 i = 0; i < CONTROLLER_TYPE_COUNT; i++) {
        if (strcmp(type_str, k_controller_type_names[i]) == 0) {
          parsed = (controller_type_t)i;
          matched = true;
          break;
        }
      }
      (void)matched; /* Empty/unknown -> default_type. */
      s->controller_types[pad] = parsed;
      small_string_destroy(&buf);
    }
  }

  /* Memory cards */
  LOAD_ENUM(s->memory_card_types[0], settings_parse_memory_card_type_name,
            settings_get_memory_card_type_name,
            SETTINGS_DEFAULT_MEMORY_CARD_1_TYPE, "MemoryCards", "Card1Type");
  LOAD_ENUM(s->memory_card_types[1], settings_parse_memory_card_type_name,
            settings_get_memory_card_type_name,
            SETTINGS_DEFAULT_MEMORY_CARD_2_TYPE, "MemoryCards", "Card2Type");
  LOAD_STR(s->memory_card_paths[0], "MemoryCards", "Card1Path");
  LOAD_STR(s->memory_card_paths[1], "MemoryCards", "Card2Path");
  s->memory_card_use_playlist_title  = settings_interface_get_bool_value(si, "MemoryCards", "UsePlaylistTitle", true);
  s->memory_card_fast_forward_access = settings_interface_get_bool_value(si, "MemoryCards", "FastForwardAccess", false);

  /* achievements: not loaded yet.  We *do* load the fields so the INI
   * round-trips, but no Achievements::Reload is invoked. */
  s->achievements_enabled                 = settings_interface_get_bool_value(si, "Cheevos", "Enabled", false);
  s->achievements_hardcore_mode           = settings_interface_get_bool_value(si, "Cheevos", "ChallengeMode", false);
  s->achievements_encore_mode             = settings_interface_get_bool_value(si, "Cheevos", "EncoreMode", false);
  s->achievements_spectator_mode          = settings_interface_get_bool_value(si, "Cheevos", "SpectatorMode", false);
  s->achievements_unofficial_test_mode    = settings_interface_get_bool_value(si, "Cheevos", "UnofficialTestMode", false);
  s->achievements_use_raintegration       = settings_interface_get_bool_value(si, "Cheevos", "UseRAIntegration", false);
  s->achievements_notifications           = settings_interface_get_bool_value(si, "Cheevos", "Notifications", true);
  s->achievements_leaderboard_notifications = settings_interface_get_bool_value(si, "Cheevos", "LeaderboardNotifications", true);
  s->achievements_leaderboard_trackers    = settings_interface_get_bool_value(si, "Cheevos", "LeaderboardTrackers", true);
  s->achievements_sound_effects           = settings_interface_get_bool_value(si, "Cheevos", "SoundEffects", true);
  s->achievements_progress_indicators     = settings_interface_get_bool_value(si, "Cheevos", "ProgressIndicators", true);
  s->achievements_prefetch_badges         = settings_interface_get_bool_value(si, "Cheevos", "PrefetchBadges",
                                                                              SETTINGS_DEFAULT_ACHIEVEMENT_BADGE_PREFETCH);
  {
    small_string_t buf; small_string_init(&buf);
    settings_interface_get_string_value(si, "Cheevos", "NotificationLocation", "", &buf);
    if (!settings_parse_notification_location_name(small_string_c_str(&buf), &s->achievements_notification_location))
      s->achievements_notification_location = SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_LOCATION;
    small_string_destroy(&buf);
  }
  {
    small_string_t buf; small_string_init(&buf);
    settings_interface_get_string_value(si, "Cheevos", "IndicatorLocation", "", &buf);
    if (!settings_parse_notification_location_name(small_string_c_str(&buf), &s->achievements_indicator_location))
      s->achievements_indicator_location = SETTINGS_DEFAULT_ACHIEVEMENT_INDICATOR_LOCATION;
    small_string_destroy(&buf);
  }
  {
    small_string_t buf; small_string_init(&buf);
    settings_interface_get_string_value(si, "Cheevos", "ChallengeIndicatorMode", "", &buf);
    if (!settings_parse_achievement_challenge_indicator_mode_name(small_string_c_str(&buf),
                                                             &s->achievements_challenge_indicator_mode))
      s->achievements_challenge_indicator_mode = SETTINGS_DEFAULT_ACHIEVEMENT_CHALLENGE_INDICATOR_MODE;
    small_string_destroy(&buf);
  }
  s->achievements_notification_duration = settings_interface_get_saturated_u8(si, "Cheevos", "NotificationsDuration",
                                                                              SETTINGS_DEFAULT_ACHIEVEMENT_NOTIFICATION_TIME);
  s->achievements_leaderboard_duration  = settings_interface_get_saturated_u8(si, "Cheevos", "LeaderboardsDuration",
                                                                              SETTINGS_DEFAULT_LEADERBOARD_NOTIFICATION_TIME);
  s->achievements_notification_scale    = settings_interface_get_saturated_s16(si, "Cheevos", "NotificationScale",
                                                                               SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_AUTO);
  s->achievements_indicator_scale       = settings_interface_get_saturated_s16(si, "Cheevos", "IndicatorScale",
                                                                               SETTINGS_ACHIEVEMENT_NOTIFICATION_SCALE_AUTO);

  /* Debug (gdbserver) */
  s->enable_gdb_server = settings_interface_get_bool_value(si, "Debug", "EnableGDBServer", false);
  s->gdb_server_port   = (u16)settings_interface_get_uint_value(si, "Debug", "GDBServerPort",
                                                                SETTINGS_DEFAULT_GDB_SERVER_PORT);

  /* Texture replacements */
  s->texture_replacements.enable_texture_replacements    = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "EnableTextureReplacements", false);
  s->texture_replacements.enable_vram_write_replacements = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "EnableVRAMWriteReplacements", false);
  s->texture_replacements.always_track_uploads = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "AlwaysTrackUploads", false);
  s->texture_replacements.preload_textures      = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "PreloadTextures", false);
  s->texture_replacements.dump_textures          = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "DumpTextures", false);
  s->texture_replacements.dump_replaced_textures = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "DumpReplacedTextures", true);
  s->texture_replacements.dump_vram_writes       = settings_interface_get_bool_value(si, "TextureReplacements",
                                                            "DumpVRAMWrites", false);

  s->texture_replacements.config.dump_texture_pages       = settings_interface_get_bool_value(si, "TextureReplacements",
                                                                "DumpTexturePages", false);
  s->texture_replacements.config.dump_full_texture_pages  = settings_interface_get_bool_value(si, "TextureReplacements",
                                                                "DumpFullTexturePages", false);
  s->texture_replacements.config.dump_texture_force_alpha_channel = settings_interface_get_bool_value(si,
                                                                "TextureReplacements", "DumpTextureForceAlphaChannel", false);
  s->texture_replacements.config.dump_vram_write_force_alpha_channel = settings_interface_get_bool_value(si,
                                                                "TextureReplacements", "DumpVRAMWriteForceAlphaChannel", true);
  s->texture_replacements.config.dump_c16_textures        = settings_interface_get_bool_value(si, "TextureReplacements",
                                                                "DumpC16Textures", false);
  s->texture_replacements.config.reduce_palette_range     = settings_interface_get_bool_value(si, "TextureReplacements",
                                                                "ReducePaletteRange", true);
  s->texture_replacements.config.convert_copies_to_writes = settings_interface_get_bool_value(si, "TextureReplacements",
                                                                "ConvertCopiesToWrites", false);
  s->texture_replacements.config.replacement_scale_linear_filter = settings_interface_get_bool_value(si,
                                                                "TextureReplacements", "ReplacementScaleLinearFilter", false);
  s->texture_replacements.config.max_hash_cache_entries = settings_interface_get_uint_value(si, "TextureReplacements",
                                                            "MaxHashCacheEntries", SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_ENTRIES);
   s->texture_replacements.config.max_hash_cache_vram_usage_mb = settings_interface_get_uint_value(si,
                                                            "TextureReplacements", "MaxHashCacheVRAMUsageMB", 
                                                            SETTINGS_TR_DEFAULT_MAX_HASH_CACHE_VRAM_USAGE_MB);
   s->texture_replacements.config.max_replacement_cache_vram_usage_mb = settings_interface_get_uint_value(si,
                                                            "TextureReplacements", "MaxReplacementCacheVRAMUsage", 
                                                            SETTINGS_TR_DEFAULT_MAX_REPLACEMENT_CACHE_VRAM_MB);
  s->texture_replacements.config.max_vram_write_splits         = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "MaxVRAMWriteSplits", 0);
  s->texture_replacements.config.max_vram_write_coalesce_width = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "MaxVRAMWriteCoalesceWidth", 0);
  s->texture_replacements.config.max_vram_write_coalesce_height = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "MaxVRAMWriteCoalesceHeight", 0);
  s->texture_replacements.config.texture_dump_width_threshold  = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "DumpTextureWidthThreshold", 16);
  s->texture_replacements.config.texture_dump_height_threshold = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "DumpTextureHeightThreshold", 16);
  s->texture_replacements.config.vram_write_dump_width_threshold  = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "DumpVRAMWriteWidthThreshold", 128);
  s->texture_replacements.config.vram_write_dump_height_threshold = settings_interface_get_saturated_u16(si,
                                                            "TextureReplacements", "DumpVRAMWriteHeightThreshold", 128);

  /* PIO/SIO/PCDrv */
  LOAD_ENUM(s->pio_device_type, settings_parse_pio_device_type_name, settings_get_pio_device_type_name,
            SETTINGS_DEFAULT_PIO_DEVICE_TYPE, "PIO", "DeviceType");
  LOAD_STR(s->pio_flash_image_path, "PIO", "FlashImagePath");
  s->pio_flash_write_enable = settings_interface_get_bool_value(si, "PIO", "FlashImageWriteEnable", false);
  s->pio_switch_active      = settings_interface_get_bool_value(si, "PIO", "SwitchActive", true);
  s->sio_redirect_to_tty    = settings_interface_get_bool_value(si, "SIO", "RedirectToTTY", false);
  s->pcdrv_enable           = settings_interface_get_bool_value(si, "PCDrv", "Enabled", false);
  s->pcdrv_enable_writes    = settings_interface_get_bool_value(si, "PCDrv", "EnableWrites", false);
  LOAD_STR(s->pcdrv_root, "PCDrv", "Root");
}

void settings_save(const settings_t* s, settings_interface_t* si, bool ignore_base)
{
  /* The "controller_types[]" string lookup table for save (mirrors the load
   * table). */
  static const char* const k_controller_type_names[] = {
    "None", "DigitalController", "AnalogController", "AnalogJoystick",
    "GunCon", "PlayStationMouse", "NeGcon", "NeGconRumble", "Justifier",
    "PopnController", "DDGoController", "Jogcon", 
  };

  settings_interface_set_string_value(si, "Console", "Region", settings_get_console_region_name(s->region));
  settings_interface_set_bool_value(si, "Console", "Enable8MBRAM", s->cpu_enable_8mb_ram);

  settings_interface_set_float_value(si, "Main", "EmulationSpeed", s->emulation_speed);
  settings_interface_set_float_value(si, "Main", "FastForwardSpeed", s->fast_forward_speed);
  settings_interface_set_float_value(si, "Main", "TurboSpeed", s->turbo_speed);

  if (!ignore_base) {
    settings_interface_set_bool_value(si, "Main", "SyncToHostRefreshRate", s->sync_to_host_refresh_rate);
    settings_interface_set_bool_value(si, "Main", "InhibitScreensaver", s->inhibit_screensaver);
    settings_interface_set_bool_value(si, "Main", "PauseOnFocusLoss", s->pause_on_focus_loss);
    settings_interface_set_bool_value(si, "Main", "PauseOnControllerDisconnection", s->pause_on_controller_disconnection);
    settings_interface_set_bool_value(si, "Main", "SaveStateOnExit", s->save_state_on_exit);
    settings_interface_set_bool_value(si, "Main", "CreateSaveStateBackups", s->create_save_state_backups);
    settings_interface_set_string_value(si, "Main", "SaveStateCompression",
                                        settings_get_save_state_compression_mode_name(s->save_state_compression));
    settings_interface_set_bool_value(si, "Main", "ConfirmPowerOff", s->confim_power_off);
    settings_interface_set_bool_value(si, "Main", "EnableDiscordPresence", s->enable_discord_presence);
  }

  settings_interface_set_bool_value(si, "Main", "DisableBackgroundInput", s->disable_background_input);
  settings_interface_set_bool_value(si, "Main", "LoadDevicesFromSaveStates", s->load_devices_from_save_states);
  settings_interface_set_bool_value(si, "Main", "DisableAllEnhancements", s->disable_all_enhancements);
  settings_interface_set_bool_value(si, "Main", "RewindEnable", s->rewind_enable);
  settings_interface_set_float_value(si, "Main", "RewindFrequency", s->rewind_save_frequency);
  settings_interface_set_uint_value(si, "Main", "RewindSaveSlots", s->rewind_save_slots);
  settings_interface_set_uint_value(si, "Main", "RunaheadFrameCount", s->runahead_frames);
  settings_interface_set_bool_value(si, "Main", "RunaheadForAnalogInput", s->runahead_for_analog_input);

  settings_interface_set_string_value(si, "CPU", "ExecutionMode", settings_get_cpu_execution_mode_name(s->cpu_execution_mode));
  settings_interface_set_bool_value(si, "CPU", "OverclockEnable", s->cpu_overclock_enable);
  settings_interface_set_int_value(si, "CPU", "OverclockNumerator",  (s32)s->cpu_overclock_numerator);
  settings_interface_set_int_value(si, "CPU", "OverclockDenominator",(s32)s->cpu_overclock_denominator);
  settings_interface_set_bool_value(si, "CPU", "RecompilerMemoryExceptions", s->cpu_recompiler_memory_exceptions);
  settings_interface_set_bool_value(si, "CPU", "RecompilerBlockLinking", s->cpu_recompiler_block_linking);
  settings_interface_set_bool_value(si, "CPU", "RecompilerICache", s->cpu_recompiler_icache);
  settings_interface_set_bool_value(si, "CPU", "RecompilerCompareRegisterFiles", s->cpu_recompiler_compare_register_files);
  settings_interface_set_string_value(si, "CPU", "FastmemMode", settings_get_cpu_fastmem_mode_name(s->cpu_fastmem_mode));

  settings_interface_set_string_value(si, "GPU", "Renderer", settings_get_renderer_name(s->gpu_renderer));
  settings_interface_set_string_value(si, "GPU", "Adapter", settings_str(s->gpu_adapter));
  settings_interface_set_string_value(si, "GPU", "UseISA",  settings_str(s->gpu_sw_use_isa));
  settings_interface_set_uint_value(si, "GPU", "ResolutionScale", s->gpu_resolution_scale);
  settings_interface_set_uint_value(si, "GPU", "Multisamples", s->gpu_multisamples);

  if (!ignore_base) {
    settings_interface_set_bool_value(si, "GPU", "UseDebugDevice", s->gpu_use_debug_device);
    settings_interface_set_bool_value(si, "GPU", "UseGPUBasedValidation", s->gpu_use_debug_device_gpu_validation);
    settings_interface_set_bool_value(si, "GPU", "PreferGLESContext", s->gpu_prefer_gles_context);
    settings_interface_set_bool_value(si, "GPU", "DisableShaderCache", s->gpu_disable_shader_cache);
    settings_interface_set_bool_value(si, "GPU", "DisableDualSourceBlend", s->gpu_disable_dual_source_blend);
    settings_interface_set_bool_value(si, "GPU", "DisableFramebufferFetch", s->gpu_disable_framebuffer_fetch);
    settings_interface_set_bool_value(si, "GPU", "DisableTextureBuffers", s->gpu_disable_texture_buffers);
    settings_interface_set_bool_value(si, "GPU", "DisableTextureCopyToSelf", s->gpu_disable_texture_copy_to_self);
    settings_interface_set_bool_value(si, "GPU", "DisableMemoryImport", s->gpu_disable_memory_import);
    settings_interface_set_bool_value(si, "GPU", "DisableRasterOrderViews", s->gpu_disable_raster_order_views);
    settings_interface_set_bool_value(si, "GPU", "DisableComputeShaders", s->gpu_disable_compute_shaders);
    settings_interface_set_bool_value(si, "GPU", "DisableCompressedTextures", s->gpu_disable_compressed_textures);
  }

  settings_interface_set_bool_value(si, "GPU", "PerSampleShading", s->gpu_per_sample_shading);
  settings_interface_set_uint_value(si, "GPU", "MaxQueuedFrames", s->gpu_max_queued_frames);
  settings_interface_set_bool_value(si, "GPU", "UseThread", s->gpu_use_thread);
  settings_interface_set_bool_value(si, "GPU", "UseSoftwareRendererForReadbacks", s->gpu_use_software_renderer_for_readbacks);
  settings_interface_set_bool_value(si, "GPU", "UseSoftwareRendererForMemoryStates", s->gpu_use_software_renderer_for_memory_states);
  settings_interface_set_bool_value(si, "GPU", "ScaledInterlacing", s->gpu_scaled_interlacing);
  settings_interface_set_bool_value(si, "GPU", "ForceRoundTextureCoordinates", s->gpu_force_round_texcoords);
  settings_interface_set_string_value(si, "GPU", "TextureFilter", settings_get_texture_filter_name(s->gpu_texture_filter));
  settings_interface_set_string_value(si, "GPU", "SpriteTextureFilter", settings_get_texture_filter_name(s->gpu_sprite_texture_filter));
  settings_interface_set_string_value(si, "GPU", "DitheringMode", settings_get_gpu_dithering_mode_name(s->gpu_dithering_mode));
  settings_interface_set_string_value(si, "GPU", "LineDetectMode", settings_get_line_detect_mode_name(s->gpu_line_detect_mode));
  settings_interface_set_string_value(si, "GPU", "DownsampleMode", settings_get_downsample_mode_name(s->gpu_downsample_mode));
  settings_interface_set_uint_value(si, "GPU", "DownsampleScale", s->gpu_downsample_scale);
  settings_interface_set_string_value(si, "GPU", "WireframeMode", settings_get_gpu_wireframe_mode_name(s->gpu_wireframe_mode));
  settings_interface_set_string_value(si, "GPU", "ForceVideoTiming",
                                      settings_get_force_video_timing_name(s->gpu_force_video_timing));
  settings_interface_set_bool_value(si, "GPU", "WidescreenHack", s->gpu_widescreen_rendering);
  settings_interface_set_bool_value(si, "GPU", "EnableModulationCrop", s->gpu_modulation_crop);
  settings_interface_set_bool_value(si, "GPU", "EnableTextureCache", s->gpu_texture_cache);
  settings_interface_set_bool_value(si, "GPU", "ChromaSmoothing24Bit", s->display_24bit_chroma_smoothing);
  settings_interface_set_bool_value(si, "GPU", "PGXPEnable", s->gpu_pgxp_enable);
  settings_interface_set_bool_value(si, "GPU", "PGXPCulling", s->gpu_pgxp_culling);
  settings_interface_set_bool_value(si, "GPU", "PGXPTextureCorrection", s->gpu_pgxp_texture_correction);
  settings_interface_set_bool_value(si, "GPU", "PGXPColorCorrection", s->gpu_pgxp_color_correction);
  settings_interface_set_bool_value(si, "GPU", "PGXPVertexCache", s->gpu_pgxp_vertex_cache);
  settings_interface_set_bool_value(si, "GPU", "PGXPCPU", s->gpu_pgxp_cpu);
  settings_interface_set_bool_value(si, "GPU", "PGXPPreserveProjFP", s->gpu_pgxp_preserve_proj_fp);
  settings_interface_set_float_value(si, "GPU", "PGXPTolerance", s->gpu_pgxp_tolerance);
  settings_interface_set_bool_value(si, "GPU", "PGXPDepthBuffer", s->gpu_pgxp_depth_buffer);
  settings_interface_set_bool_value(si, "GPU", "PGXPDisableOn2DPolygons", s->gpu_pgxp_disable_2d);
  settings_interface_set_bool_value(si, "GPU", "PGXPTransparentDepthTest", s->gpu_pgxp_transparent_depth);
  settings_interface_set_float_value(si, "GPU", "PGXPDepthThreshold", settings_get_pgxp_depth_clear_threshold(s));
  settings_interface_set_bool_value(si, "Debug", "ShowVRAM", s->gpu_show_vram);
  settings_interface_set_bool_value(si, "Debug", "DumpCPUToVRAMCopies", s->gpu_dump_cpu_to_vram_copies);
  settings_interface_set_bool_value(si, "Debug", "DumpVRAMToCPUCopies", s->gpu_dump_vram_to_cpu_copies);
  settings_interface_set_bool_value(si, "GPU", "DumpFastReplayMode", s->gpu_dump_fast_replay_mode);

  settings_interface_set_string_value(si, "GPU", "DeinterlacingMode",
                                      settings_get_display_deinterlacing_mode_name(s->display_deinterlacing_mode));
  settings_interface_set_string_value(si, "Display", "CropMode", settings_get_display_crop_mode_name(s->display_crop_mode));
  settings_interface_set_int_value(si, "Display", "ActiveStartOffset", s->display_active_start_offset);
  settings_interface_set_int_value(si, "Display", "ActiveEndOffset", s->display_active_end_offset);
  settings_interface_set_int_value(si, "Display", "LineStartOffset", s->display_line_start_offset);
  settings_interface_set_int_value(si, "Display", "LineEndOffset", s->display_line_end_offset);
  settings_interface_set_bool_value(si, "Display", "Force4_3For24Bit", s->display_force_4_3_for_24bit);
  {
    char buf[32];
    settings_get_display_aspect_ratio_name(s->display_aspect_ratio, buf, sizeof(buf));
    settings_interface_set_string_value(si, "Display", "AspectRatio", buf);
  }
  settings_interface_set_string_value(si, "Display", "FineCropMode",
                                      settings_get_display_fine_crop_mode_name(s->display_fine_crop_mode));
  settings_interface_set_int_value(si, "Display", "FineCropLeft",   s->display_fine_crop_amount[0]);
  settings_interface_set_int_value(si, "Display", "FineCropTop",    s->display_fine_crop_amount[1]);
  settings_interface_set_int_value(si, "Display", "FineCropRight",  s->display_fine_crop_amount[2]);
  settings_interface_set_int_value(si, "Display", "FineCropBottom", s->display_fine_crop_amount[3]);
  settings_interface_set_string_value(si, "Display", "Alignment", settings_get_display_alignment_name(s->display_alignment));
  settings_interface_set_string_value(si, "Display", "Rotation", settings_get_display_rotation_name(s->display_rotation));
  settings_interface_set_string_value(si, "Display", "Scaling", settings_get_display_scaling_name(s->display_scaling));
  settings_interface_set_string_value(si, "Display", "Scaling24Bit", settings_get_display_scaling_name(s->display_scaling_24bit));
  settings_interface_set_bool_value(si, "Display", "OptimalFramePacing", s->display_optimal_frame_pacing);
  settings_interface_set_bool_value(si, "Display", "PreFrameSleep", s->display_pre_frame_sleep);
  settings_interface_set_bool_value(si, "Display", "SkipPresentingDuplicateFrames", s->display_skip_presenting_duplicate_frames);
  settings_interface_set_float_value(si, "Display", "PreFrameSleepBuffer", s->display_pre_frame_sleep_buffer);
  settings_interface_set_bool_value(si, "Display", "VSync", s->display_vsync);
  settings_interface_set_bool_value(si, "Display", "DisableMailboxPresentation", s->display_disable_mailbox_presentation);
  settings_interface_set_string_value(si, "Display", "ExclusiveFullscreenControl",
                                      settings_get_display_exclusive_fullscreen_control_name(s->display_exclusive_fullscreen_control));
  settings_interface_set_string_value(si, "Display", "ScreenshotMode",
                                      settings_get_display_screenshot_mode_name(s->display_screenshot_mode));
  settings_interface_set_string_value(si, "Display", "ScreenshotFormat",
                                      settings_get_display_screenshot_format_name(s->display_screenshot_format));
  settings_interface_set_uint_value(si, "Display", "ScreenshotQuality", s->display_screenshot_quality);
  if (!ignore_base) {
    settings_interface_set_bool_value(si, "Display", "ShowOSDMessages", s->display_show_messages);
    settings_interface_set_bool_value(si, "Display", "AnimateOSDMessages", s->display_animate_messages);
    settings_interface_set_bool_value(si, "Display", "BlurOSDMessageBackgrounds", s->display_blur_message_backgrounds);
    settings_interface_set_bool_value(si, "Display", "ShowFPS", s->display_show_fps);
    settings_interface_set_bool_value(si, "Display", "ShowSpeed", s->display_show_speed);
    settings_interface_set_bool_value(si, "Display", "ShowResolution", s->display_show_resolution);
    settings_interface_set_bool_value(si, "Display", "ShowLatencyStatistics", s->display_show_latency_stats);
    settings_interface_set_bool_value(si, "Display", "ShowGPUStatistics", s->display_show_gpu_stats);
    settings_interface_set_bool_value(si, "Display", "ShowCPU", s->display_show_cpu_usage);
    settings_interface_set_bool_value(si, "Display", "ShowGPU", s->display_show_gpu_usage);
    settings_interface_set_bool_value(si, "Display", "ShowFrameTimes", s->display_show_frame_times);
    settings_interface_set_bool_value(si, "Display", "ShowStatusIndicators", s->display_show_status_indicators);
    settings_interface_set_bool_value(si, "Display", "ShowInputs", s->display_show_inputs);
    settings_interface_set_bool_value(si, "Display", "ShowEnhancements", s->display_show_enhancements);
    settings_interface_set_float_value(si, "Display", "OSDScale", s->display_osd_scale);
    settings_interface_set_float_value(si, "Display", "OSDMargin", s->display_osd_margin);
    for (u32 i = 0; i < 4u; i++) {
      char key[32];
      snprintf(key, sizeof(key), "OSD%sDuration", settings_get_display_osd_message_type_name(i));
      settings_interface_set_float_value(si, "Display", key, s->display_osd_message_duration[i]);
    }
    settings_interface_set_string_value(si, "Display", "OSDMessageLocation",
                                        settings_get_notification_location_name(s->display_osd_message_location));
  }

  settings_interface_set_bool_value(si, "Display", "AutoResizeWindow", s->display_auto_resize_window);

  settings_interface_set_int_value(si, "CDROM", "ReadaheadSectors", s->cdrom_readahead_sectors);
  settings_interface_set_string_value(si, "CDROM", "MechaconVersion",
                                      settings_get_cdrom_mech_version_name(s->cdrom_mechacon_version));
  settings_interface_set_bool_value(si, "CDROM", "RegionCheck", s->cdrom_region_check);
  settings_interface_set_bool_value(si, "CDROM", "SubQSkew", s->cdrom_subq_skew);
  settings_interface_set_bool_value(si, "CDROM", "LoadImageToRAM", s->cdrom_load_image_to_ram);
  settings_interface_set_bool_value(si, "CDROM", "LoadImagePatches", s->cdrom_load_image_patches);
  settings_interface_set_bool_value(si, "CDROM", "IgnoreHostSubcode", s->cdrom_ignore_host_subcode);
  settings_interface_set_bool_value(si, "CDROM", "MuteCDAudio", s->cdrom_mute_cd_audio);
  settings_interface_set_bool_value(si, "CDROM", "AutoDiscChange", s->cdrom_auto_disc_change);
  settings_interface_set_uint_value(si, "CDROM", "ReadSpeedup", s->cdrom_read_speedup);
  settings_interface_set_uint_value(si, "CDROM", "SeekSpeedup", s->cdrom_seek_speedup);
  settings_interface_set_uint_value(si, "CDROM", "MaxReadSpeedupCycles", s->cdrom_max_seek_speedup_cycles);
  settings_interface_set_uint_value(si, "CDROM", "MaxSeekSpeedupCycles", s->cdrom_max_read_speedup_cycles);
  settings_interface_set_bool_value(si, "CDROM", "DisableSpeedupOnMDEC", s->mdec_disable_cdrom_speedup);

  settings_interface_set_string_value(si, "Audio", "Backend", audio_stream_get_backend_name(s->audio_backend));
  settings_interface_set_string_value(si, "Audio", "Driver", settings_str(s->audio_driver));
  settings_interface_set_string_value(si, "Audio", "OutputDevice", settings_str(s->audio_output_device));
  audio_params_save(&s->audio_stream_parameters, si, "Audio");
  settings_interface_set_uint_value(si, "Audio", "OutputVolume", s->audio_output_volume);
  settings_interface_set_uint_value(si, "Audio", "FastForwardVolume", s->audio_fast_forward_volume);
  settings_interface_set_bool_value(si, "Audio", "OutputMuted", s->audio_output_muted);

  settings_interface_set_bool_value(si, "Hacks", "UseOldMDECRoutines", s->mdec_use_old_routines);
  settings_interface_set_bool_value(si, "Hacks", "ExportSharedMemory", s->export_shared_memory);
  if (!ignore_base) {
    settings_interface_set_int_value(si, "Hacks", "DMAMaxSliceTicks", s->dma_max_slice_ticks);
    settings_interface_set_int_value(si, "Hacks", "DMAHaltTicks", s->dma_halt_ticks);
    settings_interface_set_int_value(si, "Hacks", "GPUFIFOSize", (s32)s->gpu_fifo_size);
    settings_interface_set_int_value(si, "Hacks", "GPUMaxRunAhead", s->gpu_max_run_ahead);
  }

  settings_interface_set_bool_value(si, "BIOS", "TTYLogging", s->bios_tty_logging);
  settings_interface_set_bool_value(si, "BIOS", "PatchFastBoot", s->bios_patch_fast_boot);
  settings_interface_set_bool_value(si, "BIOS", "FastForwardBoot", s->bios_fast_forward_boot);

  /* Per-pad TYPE only; bindings/hotkeys: not loaded yet. */
  for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
    char section[16];
    settings_get_controller_settings_section(i, section, sizeof(section));
    const u32 type_idx = (u32)s->controller_types[i];
    settings_interface_set_string_value(si, section, "Type",
                                        type_idx < CONTROLLER_TYPE_COUNT ? k_controller_type_names[type_idx] : "None");
  }

  settings_interface_set_string_value(si, "MemoryCards", "Card1Type",
                                      settings_get_memory_card_type_name(s->memory_card_types[0]));
  settings_interface_set_string_value(si, "MemoryCards", "Card2Type",
                                      settings_get_memory_card_type_name(s->memory_card_types[1]));
  if (s->memory_card_paths[0] && s->memory_card_paths[0][0])
    settings_interface_set_string_value(si, "MemoryCards", "Card1Path", s->memory_card_paths[0]);
  else
    settings_interface_delete_value(si, "MemoryCards", "Card1Path");
  if (s->memory_card_paths[1] && s->memory_card_paths[1][0])
    settings_interface_set_string_value(si, "MemoryCards", "Card2Path", s->memory_card_paths[1]);
  else
    settings_interface_delete_value(si, "MemoryCards", "Card2Path");
  settings_interface_set_bool_value(si, "MemoryCards", "UsePlaylistTitle", s->memory_card_use_playlist_title);
  settings_interface_set_bool_value(si, "MemoryCards", "FastForwardAccess", s->memory_card_fast_forward_access);
  settings_interface_set_string_value(si, "ControllerPorts", "MultitapMode",
                                      settings_get_multitap_mode_name(s->multitap_mode));

  settings_interface_set_bool_value(si, "Cheevos", "Enabled", s->achievements_enabled);
  settings_interface_set_bool_value(si, "Cheevos", "ChallengeMode", s->achievements_hardcore_mode);
  settings_interface_set_bool_value(si, "Cheevos", "EncoreMode", s->achievements_encore_mode);
  settings_interface_set_bool_value(si, "Cheevos", "SpectatorMode", s->achievements_spectator_mode);
  settings_interface_set_bool_value(si, "Cheevos", "UnofficialTestMode", s->achievements_unofficial_test_mode);
  settings_interface_set_bool_value(si, "Cheevos", "UseRAIntegration", s->achievements_use_raintegration);
  settings_interface_set_bool_value(si, "Cheevos", "Notifications", s->achievements_notifications);
  settings_interface_set_bool_value(si, "Cheevos", "LeaderboardNotifications", s->achievements_leaderboard_notifications);
  settings_interface_set_bool_value(si, "Cheevos", "LeaderboardTrackers", s->achievements_leaderboard_trackers);
  settings_interface_set_bool_value(si, "Cheevos", "SoundEffects", s->achievements_sound_effects);
  settings_interface_set_bool_value(si, "Cheevos", "ProgressIndicators", s->achievements_progress_indicators);
  settings_interface_set_bool_value(si, "Cheevos", "PrefetchBadges", s->achievements_prefetch_badges);
  settings_interface_set_string_value(si, "Cheevos", "NotificationLocation",
                                      settings_get_notification_location_name(s->achievements_notification_location));
  settings_interface_set_string_value(si, "Cheevos", "IndicatorLocation",
                                      settings_get_notification_location_name(s->achievements_indicator_location));
  settings_interface_set_string_value(si, "Cheevos", "ChallengeIndicatorMode",
                                      settings_get_achievement_challenge_indicator_mode_name(s->achievements_challenge_indicator_mode));
  settings_interface_set_uint_value(si, "Cheevos", "NotificationsDuration", s->achievements_notification_duration);
  settings_interface_set_uint_value(si, "Cheevos", "LeaderboardsDuration", s->achievements_leaderboard_duration);
  settings_interface_set_int_value(si, "Cheevos", "NotificationScale", s->achievements_notification_scale);
  settings_interface_set_int_value(si, "Cheevos", "IndicatorScale", s->achievements_indicator_scale);

  settings_interface_set_bool_value(si, "Debug", "EnableGDBServer", s->enable_gdb_server);
  settings_interface_set_uint_value(si, "Debug", "GDBServerPort", s->gdb_server_port);

  /* Texture replacements */
  settings_interface_set_bool_value(si, "TextureReplacements", "EnableTextureReplacements",
                                    s->texture_replacements.enable_texture_replacements);
  settings_interface_set_bool_value(si, "TextureReplacements", "EnableVRAMWriteReplacements",
                                    s->texture_replacements.enable_vram_write_replacements);
  settings_interface_set_bool_value(si, "TextureReplacements", "AlwaysTrackUploads",
                                    s->texture_replacements.always_track_uploads);
  settings_interface_set_bool_value(si, "TextureReplacements", "PreloadTextures",
                                    s->texture_replacements.preload_textures);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpVRAMWrites",
                                    s->texture_replacements.dump_vram_writes);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpTextures",
                                    s->texture_replacements.dump_textures);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpReplacedTextures",
                                    s->texture_replacements.dump_replaced_textures);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpTexturePages",
                                    s->texture_replacements.config.dump_texture_pages);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpFullTexturePages",
                                    s->texture_replacements.config.dump_full_texture_pages);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpTextureForceAlphaChannel",
                                    s->texture_replacements.config.dump_texture_force_alpha_channel);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpVRAMWriteForceAlphaChannel",
                                    s->texture_replacements.config.dump_vram_write_force_alpha_channel);
  settings_interface_set_bool_value(si, "TextureReplacements", "DumpC16Textures",
                                    s->texture_replacements.config.dump_c16_textures);
  settings_interface_set_bool_value(si, "TextureReplacements", "ReducePaletteRange",
                                    s->texture_replacements.config.reduce_palette_range);
  settings_interface_set_bool_value(si, "TextureReplacements", "ConvertCopiesToWrites",
                                    s->texture_replacements.config.convert_copies_to_writes);
  settings_interface_set_bool_value(si, "TextureReplacements", "ReplacementScaleLinearFilter",
                                    s->texture_replacements.config.replacement_scale_linear_filter);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxHashCacheEntries",
                                    s->texture_replacements.config.max_hash_cache_entries);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxHashCacheVRAMUsageMB",
                                    s->texture_replacements.config.max_hash_cache_vram_usage_mb);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxReplacementCacheVRAMUsage",
                                    s->texture_replacements.config.max_replacement_cache_vram_usage_mb);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxVRAMWriteSplits",
                                    s->texture_replacements.config.max_vram_write_splits);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxVRAMWriteCoalesceWidth",
                                    s->texture_replacements.config.max_vram_write_coalesce_width);
  settings_interface_set_uint_value(si, "TextureReplacements", "MaxVRAMWriteCoalesceHeight",
                                    s->texture_replacements.config.max_vram_write_coalesce_height);
  settings_interface_set_uint_value(si, "TextureReplacements", "DumpTextureWidthThreshold",
                                    s->texture_replacements.config.texture_dump_width_threshold);
  settings_interface_set_uint_value(si, "TextureReplacements", "DumpTextureHeightThreshold",
                                    s->texture_replacements.config.texture_dump_height_threshold);
  settings_interface_set_uint_value(si, "TextureReplacements", "DumpVRAMWriteWidthThreshold",
                                    s->texture_replacements.config.vram_write_dump_width_threshold);
  settings_interface_set_uint_value(si, "TextureReplacements", "DumpVRAMWriteHeightThreshold",
                                    s->texture_replacements.config.vram_write_dump_height_threshold);

  settings_interface_set_string_value(si, "PIO", "DeviceType",
                                      settings_get_pio_device_type_name(s->pio_device_type));
  settings_interface_set_string_value(si, "PIO", "FlashImagePath", settings_str(s->pio_flash_image_path));
  settings_interface_set_bool_value(si, "PIO", "FlashImageWriteEnable", s->pio_flash_write_enable);
  settings_interface_set_bool_value(si, "PIO", "SwitchActive", s->pio_switch_active);
  settings_interface_set_bool_value(si, "SIO", "RedirectToTTY", s->sio_redirect_to_tty);
  settings_interface_set_bool_value(si, "PCDrv", "Enabled", s->pcdrv_enable);
  settings_interface_set_bool_value(si, "PCDrv", "EnableWrites", s->pcdrv_enable_writes);
  settings_interface_set_string_value(si, "PCDrv", "Root", settings_str(s->pcdrv_root));
}

void settings_apply_setting_restrictions(settings_t* s)
{
  if (!s->disable_all_enhancements)
    return;

  s->region                       = CONSOLE_REGION_AUTO;
  s->cpu_overclock_enable         = false;
  s->cpu_overclock_active         = false;
  s->cpu_enable_8mb_ram           = false;
  s->gpu_resolution_scale         = 1;
  s->gpu_multisamples             = 1;
  s->gpu_automatic_resolution_scale = false;
  s->gpu_per_sample_shading       = false;
  s->gpu_scaled_interlacing       = false;
  s->gpu_force_round_texcoords    = false;
  s->gpu_texture_filter           = GPU_TEXTURE_FILTER_NEAREST;
  s->gpu_sprite_texture_filter    = GPU_TEXTURE_FILTER_NEAREST;
  s->gpu_dithering_mode           = GPU_DITHERING_MODE_UNSCALED;
  s->gpu_line_detect_mode         = GPU_LINE_DETECT_MODE_DISABLED;
  s->gpu_downsample_mode          = GPU_DOWNSAMPLE_MODE_DISABLED;
  s->gpu_wireframe_mode           = GPU_WIREFRAME_MODE_DISABLED;
  s->gpu_force_video_timing       = FORCE_VIDEO_TIMING_MODE_DISABLED;
  s->gpu_widescreen_rendering     = false;
  s->gpu_widescreen_hack          = false;
  s->gpu_modulation_crop          = false;
  s->gpu_texture_cache            = false;
  s->gpu_pgxp_enable              = false;
  s->display_deinterlacing_mode   = DISPLAY_DEINTERLACING_MODE_ADAPTIVE;
  s->display_24bit_chroma_smoothing = false;
  s->cdrom_read_speedup           = 1;
  s->cdrom_seek_speedup           = 1;
  s->cdrom_mute_cd_audio          = false;
  s->cdrom_region_check           = false;
  s->cdrom_subq_skew              = false;
  s->cdrom_mechacon_version       = SETTINGS_DEFAULT_CDROM_MECHACON_VERSION;
  s->apply_compatibility_settings = true;
  s->texture_replacements.enable_vram_write_replacements = false;
  s->mdec_use_old_routines        = false;
  s->bios_patch_fast_boot         = false;
  s->runahead_frames              = 0;
  s->runahead_for_analog_input    = false;
  s->rewind_enable                = false;
  s->pio_device_type              = PIO_DEVICE_TYPE_NONE;
  s->pcdrv_enable                 = false;
  s->dma_max_slice_ticks          = SETTINGS_DEFAULT_DMA_MAX_SLICE_TICKS;
  s->dma_halt_ticks               = SETTINGS_DEFAULT_DMA_HALT_TICKS;
  s->gpu_fifo_size                = SETTINGS_DEFAULT_GPU_FIFO_SIZE;
  s->gpu_max_run_ahead            = SETTINGS_DEFAULT_GPU_MAX_RUN_AHEAD;
}
