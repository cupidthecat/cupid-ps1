/*
 * GameDatabase::Entry::ApplySettings.
 *
 * For each trait/optional override, the C++ source either flips a settings
 * field, accumulates a human-readable "compatibility settings have been
 * applied" message, or logs a warning.  We collapse the OSD message
 * accumulator into INFO_LOG calls gated on display_osd_messages; the
 * cupid-ps1 frontend doesn't have an icon-OSD layer yet, so all per-trait
 * flips simply log inline rather than emitting a single bulleted summary
 * at the end.
 *
 * Field/enum availability: every settings_t field referenced here is
 * already present in core/settings.h (verified by the porting subagent
 * before writing this file); no scaffolding fields were added.
 */

#include "core/game_database.h"

#include "core/controller.h"
#include "core/settings.h"
#include "core/types.h"

#include "common/log.h"
#include "common/types.h"

#include <stdbool.h>

LOG_CHANNEL(GameDatabase);

static inline bool gda_is_using_true_color(const settings_t* s)
{
  return s->gpu_dithering_mode == GPU_DITHERING_MODE_TRUE_COLOR ||
         s->gpu_dithering_mode == GPU_DITHERING_MODE_TRUE_COLOR_FULL;
}
static inline bool gda_is_using_dithering(const settings_t* s)
{
  /* Any non-disabled, non-true-color mode counts as dithering for the */
     /* purposes of the trait switch (matches Settings::IsUsingDithering on the */
  return s->gpu_dithering_mode != GPU_DITHERING_MODE_TRUE_COLOR &&
         s->gpu_dithering_mode != GPU_DITHERING_MODE_TRUE_COLOR_FULL;
}
static inline bool gda_is_using_shader_blending(const settings_t* s)
{
  return s->gpu_dithering_mode == GPU_DITHERING_MODE_SCALED_SHADER_BLEND ||
         s->gpu_dithering_mode == GPU_DITHERING_MODE_UNSCALED_SHADER_BLEND;
}
static inline bool gda_using_pgxp_cpu_mode(const settings_t* s)
{
  return s->gpu_pgxp_enable && s->gpu_pgxp_cpu;
}

static inline u16 gda_bit_for(controller_type_t t)
{
  return (u16)(1u << (u32)t);
}

void game_database_entry_apply_settings(const game_database_entry_t* e,
                                        settings_t* settings,
                                        bool display_osd_messages)
{

  if (e->has_display_active_start_offset) {
    settings->display_active_start_offset = e->display_active_start_offset;
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active start offset set to %d.",
               (int)settings->display_active_start_offset);
  }
  if (e->has_display_active_end_offset) {
    settings->display_active_end_offset = e->display_active_end_offset;
    if (display_osd_messages)
      INFO_LOG("GameDB: Display active end offset set to %d.",
               (int)settings->display_active_end_offset);
  }
  if (e->has_display_line_start_offset) {
    settings->display_line_start_offset = e->display_line_start_offset;
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line start offset set to %d.",
               (int)settings->display_line_start_offset);
  }
  if (e->has_display_line_end_offset) {
    settings->display_line_end_offset = e->display_line_end_offset;
    if (display_osd_messages)
      INFO_LOG("GameDB: Display line end offset set to %d.",
               (int)settings->display_line_end_offset);
  }
  if (e->has_dma_max_slice_ticks) {
    settings->dma_max_slice_ticks = (tick_count_t)e->dma_max_slice_ticks;
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA max slice ticks set to %u.",
               (unsigned)settings->dma_max_slice_ticks);
  }
  if (e->has_dma_halt_ticks) {
    settings->dma_halt_ticks = (tick_count_t)e->dma_halt_ticks;
    if (display_osd_messages)
      INFO_LOG("GameDB: DMA halt ticks set to %u.",
               (unsigned)settings->dma_halt_ticks);
  }
  if (e->has_cdrom_max_seek_speedup_cycles && g_settings.cdrom_seek_speedup == 0) {
    settings->cdrom_max_seek_speedup_cycles = e->cdrom_max_seek_speedup_cycles;
    if (display_osd_messages)
      INFO_LOG("GameDB: CDROM maximum seek speedup cycles set to %u.",
               (unsigned)settings->cdrom_max_seek_speedup_cycles);
  }
  if (e->has_cdrom_max_read_speedup_cycles && g_settings.cdrom_read_speedup == 0) {
    settings->cdrom_max_read_speedup_cycles = e->cdrom_max_read_speedup_cycles;
    if (display_osd_messages)
      INFO_LOG("GameDB: CDROM maximum read speedup cycles set to %u.",
               (unsigned)settings->cdrom_max_read_speedup_cycles);
  }
  if (e->has_gpu_fifo_size) {
    settings->gpu_fifo_size = e->gpu_fifo_size;
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU FIFO size set to %u.",
               (unsigned)settings->gpu_fifo_size);
  }
  if (e->has_gpu_max_run_ahead) {
    settings->gpu_max_run_ahead = (tick_count_t)e->gpu_max_run_ahead;
    if (display_osd_messages)
      INFO_LOG("GameDB: GPU max runahead set to %u.",
               (unsigned)settings->gpu_max_run_ahead);
  }
  if (e->has_gpu_pgxp_tolerance) {
    settings->gpu_pgxp_tolerance = e->gpu_pgxp_tolerance;
    if (display_osd_messages)
      INFO_LOG("GameDB: PGXP tolerance set to %f.",
               (double)settings->gpu_pgxp_tolerance);
  }
  if (e->has_gpu_pgxp_depth_threshold) {
    settings_set_pgxp_depth_clear_threshold(settings, e->gpu_pgxp_depth_threshold);
    if (display_osd_messages)
      INFO_LOG("GameDB: PGXP depth clear threshold set to %f.",
               (double)settings_get_pgxp_depth_clear_threshold(settings));
  }
  if (e->has_gpu_line_detect_mode) {
    settings->gpu_line_detect_mode = e->gpu_line_detect_mode;
    if (display_osd_messages) {
      INFO_LOG("GameDB: GPU line detect mode set to %s.",
               settings_get_line_detect_mode_name(settings->gpu_line_detect_mode));
    }
  }
  if (e->has_cpu_overclock &&
      (!settings->cpu_overclock_enable || settings->disable_all_enhancements)) {
    settings_set_cpu_overclock_percent(settings, e->cpu_overclock);
    settings->cpu_overclock_enable = true;
    settings->cpu_overclock_active = true;
    if (display_osd_messages)
      INFO_LOG("GameDB: CPU overclock set to %u.", (unsigned)e->cpu_overclock);
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_INTERPRETER)) {
    if (display_osd_messages && settings->cpu_execution_mode != CPU_EXECUTION_MODE_INTERPRETER)
      INFO_LOG("GameDB OSD: CPU recompiler disabled.");
    settings->cpu_execution_mode = CPU_EXECUTION_MODE_INTERPRETER;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_FULL_BOOT)) {
    if (display_osd_messages && settings->bios_patch_fast_boot)
      INFO_LOG("GameDB OSD: Fast boot disabled.");
    settings->bios_patch_fast_boot = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_MULTITAP)) {
    if (display_osd_messages && settings->multitap_mode != MULTITAP_MODE_DISABLED)
      INFO_LOG("GameDB OSD: Multitap disabled.");
    settings->multitap_mode = MULTITAP_MODE_DISABLED;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_FAST_FORWARD_MEMORY_CARD_ACCESS) &&
      g_settings.memory_card_fast_forward_access) {
    if (display_osd_messages)
      INFO_LOG("GameDB OSD: Fast forward memory card access disabled.");
    settings->memory_card_fast_forward_access = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_CDROM_READ_SPEEDUP)) {
    if (settings->cdrom_read_speedup != 1)
      INFO_LOG("GameDB OSD: CD-ROM read speedup disabled.");
    settings->cdrom_read_speedup = 1;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_CDROM_SEEK_SPEEDUP)) {
    if (settings->cdrom_seek_speedup != 1)
      INFO_LOG("GameDB OSD: CD-ROM seek speedup disabled.");
    settings->cdrom_seek_speedup = 1;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_CDROM_SPEEDUP_ON_MDEC)) {
    WARNING_LOG("Disabling CD-ROM speedup on MDEC.");
    settings->mdec_disable_cdrom_speedup = true;
  } else if (settings->mdec_disable_cdrom_speedup && settings->cdrom_read_speedup != 1) {
    WARNING_LOG("Disable CD-ROM speedup on MDEC is enabled, "
                "but it is not required for this game.");
  }

  if (e->has_display_crop_mode) {
    if (display_osd_messages && settings->display_crop_mode != e->display_crop_mode) {
      INFO_LOG("GameDB OSD: Display cropping set to %s.",
               settings_get_display_crop_mode_display_name(e->display_crop_mode));
    }
    settings->display_crop_mode = e->display_crop_mode;
  } else if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_ALL_BORDERS_CROP) &&
             settings->display_crop_mode >= DISPLAY_CROP_MODE_BORDERS &&
             settings->display_crop_mode <= DISPLAY_CROP_MODE_BORDERS_UNCORRECTED) {
    const display_crop_mode_t new_mode = DISPLAY_CROP_MODE_OVERSCAN;
    if (display_osd_messages) {
      INFO_LOG("GameDB OSD: Display cropping set to %s.",
               settings_get_display_crop_mode_display_name(new_mode));
    }
    settings->display_crop_mode = new_mode;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_SOFTWARE_RENDERER)) {
    if (display_osd_messages && settings->gpu_renderer != GPU_RENDERER_SOFTWARE)
      INFO_LOG("GameDB OSD: Hardware rendering disabled.");
    settings->gpu_renderer = GPU_RENDERER_SOFTWARE;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_SOFTWARE_RENDERER_FOR_READBACKS)) {
    if (display_osd_messages && settings->gpu_renderer != GPU_RENDERER_SOFTWARE)
      INFO_LOG("GameDB OSD: Software renderer readbacks enabled.");
    settings->gpu_use_software_renderer_for_readbacks = true;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_ROUND_UPSCALED_TEXTURE_COORDINATES)) {
    settings->gpu_force_round_texcoords = true;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_DEINTERLACING)) {
    /* If the entry overrides the deinterlacing mode, use it; otherwise pick a
       reasonable non-disabled, non-progressive mode; preserving the user's
       choice when they already have one. */
    display_deinterlacing_mode_t new_mode;
    if (e->has_display_deinterlacing_mode) {
      new_mode = e->display_deinterlacing_mode;
    } else if (settings->display_deinterlacing_mode != DISPLAY_DEINTERLACING_MODE_DISABLED &&
               settings->display_deinterlacing_mode != DISPLAY_DEINTERLACING_MODE_PROGRESSIVE) {
      new_mode = settings->display_deinterlacing_mode;
    } else {
      new_mode = SETTINGS_DEFAULT_DISPLAY_DEINTERLACING_MODE;
    }
    if (display_osd_messages && settings->display_deinterlacing_mode != new_mode) {
      INFO_LOG("GameDB OSD: Deinterlacing set to %s.",
               settings_get_display_deinterlacing_mode_display_name(new_mode));
    }
    settings->display_deinterlacing_mode = new_mode;
  } else if (e->has_display_deinterlacing_mode) {
    /* Preserve "Progressive" if the user picked it. */
    if (settings->display_deinterlacing_mode != DISPLAY_DEINTERLACING_MODE_PROGRESSIVE) {
      if (display_osd_messages &&
          settings->display_deinterlacing_mode != e->display_deinterlacing_mode) {
        INFO_LOG("GameDB OSD: Deinterlacing set to %s.",
                 settings_get_display_deinterlacing_mode_display_name(e->display_deinterlacing_mode));
      }
      settings->display_deinterlacing_mode = e->display_deinterlacing_mode;
    }
  }

  /* Dithering switches: order matters. */
  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_TRUE_COLOR) ||
      game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_FULL_TRUE_COLOR) ||
      game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_SCALED_DITHERING) ||
      game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_SHADER_BLENDING) ||
      game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_FULL_TRUE_COLOR)) {
    const gpu_dithering_mode_t old_mode = settings->gpu_dithering_mode;

    if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_TRUE_COLOR) &&
        gda_is_using_true_color(settings)) {
      settings->gpu_dithering_mode = GPU_DITHERING_MODE_SCALED;
    }
    if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_SCALED_DITHERING) &&
        gda_is_using_dithering(settings)) {
       settings->gpu_dithering_mode = gda_is_using_shader_blending(settings)
                                       ? GPU_DITHERING_MODE_UNSCALED_SHADER_BLEND 
                                       : GPU_DITHERING_MODE_UNSCALED;
    }
    if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_SHADER_BLENDING) &&
        gda_is_using_dithering(settings) && !gda_is_using_shader_blending(settings)) {
       settings->gpu_dithering_mode = (settings->gpu_dithering_mode == GPU_DITHERING_MODE_SCALED)
                                       ? GPU_DITHERING_MODE_SCALED_SHADER_BLEND 
                                       : GPU_DITHERING_MODE_UNSCALED_SHADER_BLEND;
    }
    if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_FULL_TRUE_COLOR) &&
        settings->gpu_dithering_mode == GPU_DITHERING_MODE_TRUE_COLOR) {
      settings->gpu_dithering_mode = GPU_DITHERING_MODE_TRUE_COLOR_FULL;
    }
    if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_FULL_TRUE_COLOR) &&
        settings->gpu_dithering_mode == GPU_DITHERING_MODE_TRUE_COLOR_FULL) {
      settings->gpu_dithering_mode = GPU_DITHERING_MODE_TRUE_COLOR;
    }

    if (display_osd_messages && settings->gpu_dithering_mode != old_mode) {
      INFO_LOG("GameDB OSD: Dithering set to %s.",
               settings_get_gpu_dithering_mode_display_name(settings->gpu_dithering_mode));
    }
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_UPSCALING)) {
    if (display_osd_messages) {
      if (settings->gpu_resolution_scale != 1)
        INFO_LOG("GameDB OSD: Upscaling disabled.");
      if (settings->gpu_multisamples != 1)
        INFO_LOG("GameDB OSD: MSAA disabled.");
    }
    settings->gpu_resolution_scale = 1;
    settings->gpu_automatic_resolution_scale = false;
    settings->gpu_multisamples = 1;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_TEXTURE_FILTERING)) {
    if (display_osd_messages &&
        (settings->gpu_texture_filter != GPU_TEXTURE_FILTER_NEAREST ||
         g_settings.gpu_sprite_texture_filter != GPU_TEXTURE_FILTER_NEAREST)) {
      INFO_LOG("GameDB OSD: Texture filtering disabled.");
    }
    settings->gpu_texture_filter = GPU_TEXTURE_FILTER_NEAREST;
    settings->gpu_sprite_texture_filter = GPU_TEXTURE_FILTER_NEAREST;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_SPRITE_TEXTURE_FILTERING)) {
    if (display_osd_messages &&
        g_settings.gpu_sprite_texture_filter != GPU_TEXTURE_FILTER_NEAREST) {
      INFO_LOG("GameDB OSD: Sprite texture filtering disabled.");
    }
    settings->gpu_sprite_texture_filter = GPU_TEXTURE_FILTER_NEAREST;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_SCALED_INTERLACING)) {
    if (display_osd_messages && settings->gpu_scaled_interlacing &&
        settings->display_deinterlacing_mode != DISPLAY_DEINTERLACING_MODE_PROGRESSIVE) {
      INFO_LOG("GameDB OSD: Scaled interlacing disabled.");
    }
    settings->gpu_scaled_interlacing = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_WIDESCREEN)) {
    if (display_osd_messages && settings->gpu_widescreen_rendering)
      INFO_LOG("GameDB OSD: Widescreen rendering disabled.");
    settings->gpu_widescreen_rendering = false;
    settings->gpu_widescreen_hack = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP)) {
    if (display_osd_messages && settings->gpu_pgxp_enable)
      INFO_LOG("GameDB OSD: PGXP geometry correction disabled.");
    settings->gpu_pgxp_enable = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP_CULLING)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && settings->gpu_pgxp_culling)
      INFO_LOG("GameDB OSD: PGXP culling correction disabled.");
    settings->gpu_pgxp_culling = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP_TEXTURE_CORRECTION)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && settings->gpu_pgxp_texture_correction)
      INFO_LOG("GameDB OSD: PGXP perspective correct textures disabled.");
    settings->gpu_pgxp_texture_correction = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP_COLOR_CORRECTION)) {
    if (display_osd_messages && settings->gpu_pgxp_enable &&
        settings->gpu_pgxp_texture_correction && settings->gpu_pgxp_color_correction) {
      INFO_LOG("GameDB OSD: PGXP perspective correct colors disabled.");
    }
    settings->gpu_pgxp_color_correction = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_PGXP_VERTEX_CACHE)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && !settings->gpu_pgxp_vertex_cache)
      INFO_LOG("GameDB OSD: PGXP vertex cache enabled.");
    settings->gpu_pgxp_vertex_cache = settings->gpu_pgxp_enable;
  } else if (settings->gpu_pgxp_enable && settings->gpu_pgxp_vertex_cache) {
    WARNING_LOG("PGXP Vertex Cache is enabled, but it is not required for this game. "
                "This may cause rendering errors.");
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_PGXP_CPU_MODE)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && !settings->gpu_pgxp_cpu) {
      INFO_LOG("GameDB OSD: PGXP CPU mode enabled.");
    }
    settings->gpu_pgxp_cpu = settings->gpu_pgxp_enable;
  } else if (gda_using_pgxp_cpu_mode(settings)) {
    WARNING_LOG("PGXP CPU mode is enabled, but it is not required for this game. "
                "This may cause rendering errors.");
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP_DEPTH_BUFFER)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && settings->gpu_pgxp_depth_buffer)
      INFO_LOG("GameDB OSD: PGXP depth buffer disabled.");
    settings->gpu_pgxp_depth_buffer = false;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_DISABLE_PGXP_ON_2D_POLYGONS)) {
    if (display_osd_messages && settings->gpu_pgxp_enable && !settings->gpu_pgxp_disable_2d)
      INFO_LOG("GameDB OSD: PGXP disabled on 2D polygons.");
    /* Write to g_settings here (not the local `settings` reference). */
    g_settings.gpu_pgxp_disable_2d = true;
  }

  if (e->has_gpu_pgxp_preserve_proj_fp) {
    if (display_osd_messages) {
      INFO_LOG("GameDB: GPU preserve projection precision set to %s.",
               e->gpu_pgxp_preserve_proj_fp ? "true" : "false");

      if (settings->gpu_pgxp_enable && settings->gpu_pgxp_preserve_proj_fp &&
          !e->gpu_pgxp_preserve_proj_fp) {
        INFO_LOG("GameDB OSD: PGXP preserve projection precision disabled.");
      }
    }
    settings->gpu_pgxp_preserve_proj_fp = e->gpu_pgxp_preserve_proj_fp;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_RECOMPILER_ICACHE)) {
    WARNING_LOG("ICache for recompiler forced by compatibility settings.");
    settings->cpu_recompiler_icache = true;
  }

  if (game_database_entry_has_trait(e, GAME_DATABASE_TRAIT_FORCE_CDROM_SUBQ_SKEW)) {
    WARNING_LOG("CD-ROM SubQ Skew forced by compatibility settings.");
    settings->cdrom_subq_skew = true;
  }

  if (e->supported_controllers != 0 && e->supported_controllers != (u16)0xFFFFu) {
    for (u32 i = 0; i < NUM_CONTROLLER_AND_CARD_PORTS; i++) {
      const controller_type_t ctype = settings->controller_types[i];
      if (ctype == CONTROLLER_TYPE_NONE)
        continue;

      if (e->supported_controllers & gda_bit_for(ctype))
        continue;

      /* Special case: AnalogController is permitted when not supported as
         long as DigitalController is supported (the dualshock falls back to
         digital mode in that case). */
      if (ctype == CONTROLLER_TYPE_ANALOG_CONTROLLER &&
          (e->supported_controllers & gda_bit_for(CONTROLLER_TYPE_DIGITAL_CONTROLLER)) != 0) {
        continue;
      }

      if (display_osd_messages) {
        const controller_info_t* current_info = controller_get_info_for_type(ctype);
        WARNING_LOG("Controller in Port %u (%s) is not supported for this game.",
                    (unsigned)(i + 1u),
                    current_info ? current_info->display_name : "unknown");

        WARNING_LOG("Please configure a supported controller from the following list:");
        for (u32 j = 0; j < (u32)CONTROLLER_TYPE_COUNT; j++) {
          const controller_type_t supported_ctype = (controller_type_t)j;
          if ((e->supported_controllers & gda_bit_for(supported_ctype)) == 0)
            continue;
          const controller_info_t* info = controller_get_info_for_type(supported_ctype);
          if (info)
            WARNING_LOG("  - %s", info->display_name);
        }
      }
    }

    if (g_settings.multitap_mode != MULTITAP_MODE_DISABLED &&
        !(e->supported_controllers & GAME_DATABASE_SUPPORTS_MULTITAP_BIT)) {
      WARNING_LOG("This game does not support multitap, but multitap is enabled. "
                  "This may result in dropped controller inputs.");
    }
  }
}
