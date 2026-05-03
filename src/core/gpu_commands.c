/*
 * GP0 / GP1 command implementations.  This TU is #included from gpu.c so we
 * have direct access to the file-static `g` state without exporting it.
 * Dispatch is a plain static const u8[] mapping opcode -> handler enum,
 * driven by gpu_dispatch_gp0().  Keeps the dispatch trivially scrutable.
 *
 * The PGXP-precise polygon/line variants are dropped (not in cupid-ps1 scope).
 */

/* All these helpers see `g`, fifo_*, gpu_*  from gpu.c. */

/* When clangd / LSP parses this file standalone (Makefile excludes it from
 * the build), pull common headers so it isn't drowning in red.  Idempotent
 * via header guards when included from gpu.c. */
#include "common/types.h"
#include "common/intrin.h"
#include <stdbool.h>

#define CHECK_COMMAND_SIZE(num_words)                              \
  do {                                                             \
    if (fifo_size(&g.fifo) < (num_words)) {                        \
      g.command_total_words = (num_words);                         \
      return false;                                                \
    }                                                              \
  } while (0)

ALWAYS_INLINE static u32 replace_zero(u32 value, u32 value_for_zero)
{
  return value == 0 ? value_for_zero : value;
}

ALWAYS_INLINE static u32 sign_extend_11(u32 v)
{
  v &= 0x7FFu;
  return (v & 0x400u) ? (v | ~0x7FFu) : v;
}

static bool gpu_handle_unknown(void);
static bool gpu_handle_nop(void);
static bool gpu_handle_clear_cache(void);
static bool gpu_handle_interrupt_request(void);
static bool gpu_handle_set_draw_mode(void);
static bool gpu_handle_set_texture_window(void);
static bool gpu_handle_set_drawing_area_top_left(void);
static bool gpu_handle_set_drawing_area_bottom_right(void);
static bool gpu_handle_set_drawing_offset(void);
static bool gpu_handle_set_mask_bit(void);
static bool gpu_handle_render_polygon(void);
static bool gpu_handle_render_rectangle(void);
static bool gpu_handle_render_line(void);
static bool gpu_handle_render_polyline(void);
static bool gpu_handle_fill_rectangle(void);
static bool gpu_handle_copy_cpu_to_vram(void);
static bool gpu_handle_copy_vram_to_cpu(void);
static bool gpu_handle_copy_vram_to_vram(void);

/* Dispatch table opcode -> handler index. */
typedef enum {
  GP0H_UNKNOWN = 0,
  GP0H_NOP,
  GP0H_CLEAR_CACHE,
  GP0H_INTERRUPT_REQUEST,
  GP0H_SET_DRAW_MODE,
  GP0H_SET_TEXTURE_WINDOW,
  GP0H_SET_DRAWING_AREA_TL,
  GP0H_SET_DRAWING_AREA_BR,
  GP0H_SET_DRAWING_OFFSET,
  GP0H_SET_MASK_BIT,
  GP0H_RENDER_POLYGON,
  GP0H_RENDER_RECTANGLE,
  GP0H_RENDER_LINE,
  GP0H_RENDER_POLYLINE,
  GP0H_FILL_RECTANGLE,
  GP0H_COPY_CPU_VRAM,
  GP0H_COPY_VRAM_CPU,
  GP0H_COPY_VRAM_VRAM,
} gp0_handler_t;

static u8 s_gp0_table[256];
static bool s_gp0_table_built = false;

static void build_gp0_table(void)
{
  for (u32 i = 0; i < 256; i++) s_gp0_table[i] = GP0H_UNKNOWN;
  s_gp0_table[0x00] = GP0H_NOP;
  s_gp0_table[0x01] = GP0H_CLEAR_CACHE;
  s_gp0_table[0x02] = GP0H_FILL_RECTANGLE;
  s_gp0_table[0x03] = GP0H_NOP;
  for (u32 i = 0x04; i <= 0x1E; i++) s_gp0_table[i] = GP0H_NOP;
  s_gp0_table[0x1F] = GP0H_INTERRUPT_REQUEST;

  for (u32 i = 0x20; i <= 0x7F; i++) {
    gpu_render_command_t rc = { (u32)(i << 24) };
    switch (gpu_render_command_primitive(rc)) {
      case GPU_PRIMITIVE_POLYGON:
        s_gp0_table[i] = GP0H_RENDER_POLYGON; break;
      case GPU_PRIMITIVE_LINE:
        s_gp0_table[i] = gpu_render_command_polyline(rc) ? GP0H_RENDER_POLYLINE : GP0H_RENDER_LINE; break;
      case GPU_PRIMITIVE_RECTANGLE:
        s_gp0_table[i] = GP0H_RENDER_RECTANGLE; break;
      default:
        s_gp0_table[i] = GP0H_UNKNOWN; break;
    }
  }
  s_gp0_table[0xE0] = GP0H_NOP;
  s_gp0_table[0xE1] = GP0H_SET_DRAW_MODE;
  s_gp0_table[0xE2] = GP0H_SET_TEXTURE_WINDOW;
  s_gp0_table[0xE3] = GP0H_SET_DRAWING_AREA_TL;
  s_gp0_table[0xE4] = GP0H_SET_DRAWING_AREA_BR;
  s_gp0_table[0xE5] = GP0H_SET_DRAWING_OFFSET;
  s_gp0_table[0xE6] = GP0H_SET_MASK_BIT;
  for (u32 i = 0xE7; i <= 0xEF; i++) s_gp0_table[i] = GP0H_NOP;
  for (u32 i = 0x80; i <= 0x9F; i++) s_gp0_table[i] = GP0H_COPY_VRAM_VRAM;
  for (u32 i = 0xA0; i <= 0xBF; i++) s_gp0_table[i] = GP0H_COPY_CPU_VRAM;
  for (u32 i = 0xC0; i <= 0xDF; i++) s_gp0_table[i] = GP0H_COPY_VRAM_CPU;
  s_gp0_table[0xFF] = GP0H_NOP;
  s_gp0_table_built = true;
}

static u32 s_dbg_op_count[256];
__attribute__((visibility("default")))
const u32* gpu_dbg_op_counts(void) { return s_dbg_op_count; }

static bool dispatch_gp0(void)
{
  if (!s_gp0_table_built) build_gp0_table();
  const u32 cmd = fifo_peek(&g.fifo, 0) >> 24;
  s_dbg_op_count[cmd & 0xFFu]++;
  switch ((gp0_handler_t)s_gp0_table[cmd]) {
    case GP0H_NOP:                   return gpu_handle_nop();
    case GP0H_CLEAR_CACHE:           return gpu_handle_clear_cache();
    case GP0H_INTERRUPT_REQUEST:     return gpu_handle_interrupt_request();
    case GP0H_SET_DRAW_MODE:         return gpu_handle_set_draw_mode();
    case GP0H_SET_TEXTURE_WINDOW:    return gpu_handle_set_texture_window();
    case GP0H_SET_DRAWING_AREA_TL:   return gpu_handle_set_drawing_area_top_left();
    case GP0H_SET_DRAWING_AREA_BR:   return gpu_handle_set_drawing_area_bottom_right();
    case GP0H_SET_DRAWING_OFFSET:    return gpu_handle_set_drawing_offset();
    case GP0H_SET_MASK_BIT:          return gpu_handle_set_mask_bit();
    case GP0H_RENDER_POLYGON:        return gpu_handle_render_polygon();
    case GP0H_RENDER_RECTANGLE:      return gpu_handle_render_rectangle();
    case GP0H_RENDER_LINE:           return gpu_handle_render_line();
    case GP0H_RENDER_POLYLINE:       return gpu_handle_render_polyline();
    case GP0H_FILL_RECTANGLE:        return gpu_handle_fill_rectangle();
    case GP0H_COPY_CPU_VRAM:         return gpu_handle_copy_cpu_to_vram();
    case GP0H_COPY_VRAM_CPU:         return gpu_handle_copy_vram_to_cpu();
    case GP0H_COPY_VRAM_VRAM:        return gpu_handle_copy_vram_to_vram();
    case GP0H_UNKNOWN:
    default:                         return gpu_handle_unknown();
  }
}

static void gpu_try_execute_commands(void)
{
  while (g.pending_command_ticks <= g.max_run_ahead && !fifo_empty(&g.fifo)) {
    switch (g.blitter_state) {
      case GPU_BLITTER_IDLE: {
        if (!dispatch_gp0()) return;
        continue;
      }
      case GPU_BLITTER_WRITING_VRAM: {
        const u32 to_copy = (g.blit_remaining_words < fifo_size(&g.fifo)) ? g.blit_remaining_words : fifo_size(&g.fifo);
        for (u32 i = 0; i < to_copy; i++)
          gpu_blit_push(fifo_pop(&g.fifo));
        g.blit_remaining_words -= to_copy;
        if (g.blit_remaining_words == 0)
          gpu_finish_vram_write();
        continue;
      }
      case GPU_BLITTER_READING_VRAM:
        return;
      case GPU_BLITTER_DRAWING_POLYLINE: {
        const u32 words_per_vertex = gpu_render_command_shading_enable(g.render_command) ? 2u : 1u;
        u32 terminator_index = gpu_render_command_shading_enable(g.render_command)
                                 ? (((u32)g.polyline_buffer.size & 1u) ^ 1u) : 0u;
        for (; terminator_index < fifo_size(&g.fifo); terminator_index += words_per_vertex) {
          if ((fifo_peek(&g.fifo, terminator_index) & 0xF000F000u) == 0x50005000u)
            break;
        }
        const bool found_terminator = terminator_index < fifo_size(&g.fifo);
        const u32 words_to_copy = (terminator_index < fifo_size(&g.fifo)) ? terminator_index : fifo_size(&g.fifo);
        for (u32 i = 0; i < words_to_copy && g.polyline_buffer.size < GPU_MAX_POLYLINE_WORDS; i++)
          g.polyline_buffer.data[g.polyline_buffer.size++] = fifo_pop_u64(&g.fifo);
        if (found_terminator) {
          fifo_remove_one(&g.fifo);   /* drop terminator */
          gpu_finish_polyline();
          g.polyline_buffer.size = 0;
          gpu_end_command();
          continue;
        }
        return;
      }
    }
  }
}

static bool gpu_handle_unknown(void)
{
  ERROR_LOG("Unimplemented GP0 command 0x%02X", fifo_peek(&g.fifo, 0) >> 24);
  fifo_remove_one(&g.fifo);
  gpu_end_command();
  return true;
}
static bool gpu_handle_nop(void)
{
  fifo_remove_one(&g.fifo);
  gpu_end_command();
  return true;
}
static bool gpu_handle_clear_cache(void)
{
  gpu_invalidate_clut();
  gpu_backend_clear_cache();
  fifo_remove_one(&g.fifo);
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_interrupt_request(void)
{
  g.GPUSTAT.bits |= (1u << 24);
  interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_GPU, true);
  fifo_remove_one(&g.fifo);
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_draw_mode(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  gpu_set_draw_mode((u16)param);
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_texture_window(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  gpu_set_texture_window(param);
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_drawing_area_top_left(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  const u32 left = param & GPU_DRAWING_AREA_COORD_MASK;
  const u32 top  = (param >> 10) & GPU_DRAWING_AREA_COORD_MASK;
  if (g.drawing_area.left != left || g.drawing_area.top != top) {
    g.drawing_area.left = left;
    g.drawing_area.top  = top;
    g.drawing_area_changed = true;
  }
  if (gpu_trace_disp_enabled()) {
    static u32 datl_n = 0;
    if ((datl_n++ % 64u) == 0u)
      fprintf(stderr, "[disp area_tl #%u] tl=(%u,%u) raw=0x%06X\n",
              datl_n, (unsigned)left, (unsigned)top, (unsigned)param);
  }
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_drawing_area_bottom_right(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  const u32 right = param & GPU_DRAWING_AREA_COORD_MASK;
  const u32 bottom = (param >> 10) & GPU_DRAWING_AREA_COORD_MASK;
  if (g.drawing_area.right != right || g.drawing_area.bottom != bottom) {
    g.drawing_area.right  = right;
    g.drawing_area.bottom = bottom;
    g.drawing_area_changed = true;
  }
  if (gpu_trace_disp_enabled()) {
    static u32 dabr_n = 0;
    if ((dabr_n++ % 64u) == 0u)
      fprintf(stderr, "[disp area_br #%u] br=(%u,%u) raw=0x%06X\n",
              dabr_n, (unsigned)right, (unsigned)bottom, (unsigned)param);
  }
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_drawing_offset(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  const s32 x = (s32)sign_extend_11(param & 0x7FFu);
  const s32 y = (s32)sign_extend_11((param >> 11) & 0x7FFu);
  g.drawing_offset.x = x;
  g.drawing_offset.y = y;
  if (gpu_trace_disp_enabled()) {
    static u32 doff_n = 0;
    if ((doff_n++ % 64u) == 0u)
      fprintf(stderr, "[disp off #%u] off=(%d,%d) raw=0x%06X\n",
              doff_n, (int)x, (int)y, (unsigned)param);
  }
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}
static bool gpu_handle_set_mask_bit(void)
{
  const u32 param = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  const u32 mask = (1u << 11) | (1u << 12);
  const u32 bits = (param & 0x3u) << 11;
  g.GPUSTAT.bits = (g.GPUSTAT.bits & ~mask) | bits;
  gpu_add_command_ticks(1);
  gpu_end_command();
  return true;
}

static bool gpu_handle_render_polygon(void)
{
  const gpu_render_command_t rc = { fifo_peek(&g.fifo, 0) };
  const u32 words_per_vertex = 1u + (gpu_render_command_texture_enable(rc) ? 1u : 0u)
                                  + (gpu_render_command_shading_enable(rc) ? 1u : 0u);
  const u32 num_vertices = gpu_render_command_quad_polygon(rc) ? 4u : 3u;
  const u32 total_words = words_per_vertex * num_vertices + (gpu_render_command_shading_enable(rc) ? 0u : 1u);
  CHECK_COMMAND_SIZE(total_words);

  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  /* Setup ticks indexed by [quad?][shaded?][textured?]. */
  static const u16 setup_time[2][2][2] = { { {46, 226}, {334, 496} }, { {82, 262}, {370, 532} } };
  gpu_add_command_ticks(setup_time[gpu_render_command_quad_polygon(rc) ? 1 : 0]
                                   [gpu_render_command_shading_enable(rc) ? 1 : 0]
                                   [gpu_render_command_texture_enable(rc) ? 1 : 0]);

  if (gpu_render_command_texture_enable(rc)) {
    const u16 texpage_attr = (u16)((gpu_render_command_shading_enable(rc) ? fifo_peek(&g.fifo, 5) : fifo_peek(&g.fifo, 4)) >> 16);
    gpu_set_draw_mode((u16)((texpage_attr & GPU_DRAW_MODE_REG_POLYGON_TEXPAGE_MASK) |
                            (g.draw_mode.mode_reg.bits & ~GPU_DRAW_MODE_REG_POLYGON_TEXPAGE_MASK)));
    gpu_set_texture_palette((u16)(fifo_peek(&g.fifo, 2) >> 16));
    gpu_update_clut_if_needed(gpu_draw_mode_reg_texture_mode(g.draw_mode.mode_reg), g.draw_mode.palette_reg);
  }

  g.render_command.bits = rc.bits;
  fifo_remove_one(&g.fifo);
  gpu_prepare_for_draw();

  const u32 first_color = gpu_render_command_color_for_first_vertex(rc);
  const bool shaded = gpu_render_command_shading_enable(rc);
  const bool textured = gpu_render_command_texture_enable(rc);

  if (g_settings.gpu_pgxp_enable) {
    /* PGXP: pop full u64 to recover RAM source MADDR; look up precise float */
    gpu_backend_precise_polygon_vertex_t pv[4];
    memset(pv, 0, sizeof(pv));
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    for (u32 i = 0; i < num_vertices; i++) {
      pv[i].color = (shaded && i > 0) ? (fifo_pop(&g.fifo) & 0x00FFFFFFu) : first_color;
      const u64 maddr_and_pos = fifo_pop_u64(&g.fifo);
      const gpu_vertex_position_t vp = { (u32)maddr_and_pos };
      pv[i].native_x = g.drawing_offset.x + gpu_vertex_position_x(vp);
      pv[i].native_y = g.drawing_offset.y + gpu_vertex_position_y(vp);
      pv[i].texcoord = textured ? (u16)fifo_pop(&g.fifo) : 0;
      valid_w &= pgxp_get_precise_vertex((u32)(maddr_and_pos >> 32), vp.bits,
                                          pv[i].native_x, pv[i].native_y,
                                          g.drawing_offset.x, g.drawing_offset.y, 
                                          &pv[i].x, &pv[i].y, &pv[i].w);
    }

    if (!valid_w) {
      if (g_settings.gpu_pgxp_disable_2d) {
        for (u32 i = 0; i < num_vertices; i++) {
          pv[i].x = (float)pv[i].native_x;
          pv[i].y = (float)pv[i].native_y;
          pv[i].w = 1.0f;
        }
      } else {
        for (u32 i = 0; i < num_vertices; i++)
          pv[i].w = 1.0f;
      }
    }

    /* Too-large primitive culling, command-side. */
    const s32 v0x = pv[0].native_x, v0y = pv[0].native_y;
    const s32 v1x = pv[1].native_x, v1y = pv[1].native_y;
    const s32 v2x = pv[2].native_x, v2y = pv[2].native_y;
    const s32 min12x = (v1x < v2x) ? v1x : v2x;
    const s32 min12y = (v1y < v2y) ? v1y : v2y;
    const s32 max12x = (v1x > v2x) ? v1x : v2x;
    const s32 max12y = (v1y > v2y) ? v1y : v2y;
    const s32 minx012 = (min12x < v0x) ? min12x : v0x;
    const s32 miny012 = (min12y < v0y) ? min12y : v0y;
    const s32 maxx012 = (max12x > v0x) ? max12x : v0x;
    const s32 maxy012 = (max12y > v0y) ? max12y : v0y;
    const bool first_tri_culled =
        ((maxx012 + 1 - minx012) > (s32)MAX_PRIMITIVE_WIDTH) ||
        ((maxy012 + 1 - miny012) > (s32)MAX_PRIMITIVE_HEIGHT);

    u32 emit_count = num_vertices;
    if (gpu_render_command_quad_polygon(rc)) {
      const s32 v3x = pv[3].native_x, v3y = pv[3].native_y;
      const s32 minq_x = (min12x < v3x) ? min12x : v3x;
      const s32 minq_y = (min12y < v3y) ? min12y : v3y;
      const s32 maxq_x = (max12x > v3x) ? max12x : v3x;
      const s32 maxq_y = (max12y > v3y) ? max12y : v3y;
      const bool second_tri_culled =
          ((maxq_x + 1 - minq_x) > (s32)MAX_PRIMITIVE_WIDTH) ||
          ((maxq_y + 1 - minq_y) > (s32)MAX_PRIMITIVE_HEIGHT);
      if (second_tri_culled) {
        if (first_tri_culled) {
          gpu_end_command();
          return true;
        }
        emit_count = 3;
      } else if (first_tri_culled) {
        /* Move second triangle (v2,v1,v3) into slot (v0,v1,v2). */
        pv[0] = pv[2];
        pv[2] = pv[3];
        emit_count = 3;
      }
    } else if (first_tri_culled) {
      gpu_end_command();
      return true;
    }

    gpu_backend_draw_precise_polygon_cmd_t pcmd;
    memset(&pcmd, 0, sizeof(pcmd));
    gpu_fill_draw_command(&pcmd.base, rc);
    pcmd.base.num_vertices = (u16)emit_count;
    pcmd.base.valid_w = valid_w;
    gpu_backend_draw_precise_polygon(&pcmd, pv);
  } else {
    gpu_backend_polygon_vertex_t vertices[4];
    memset(vertices, 0, sizeof(vertices));
    for (u32 i = 0; i < num_vertices; i++) {
      vertices[i].color = (shaded && i > 0) ? (fifo_pop(&g.fifo) & 0x00FFFFFFu) : first_color;
      const gpu_vertex_position_t vp = { fifo_pop(&g.fifo) };
      vertices[i].x = g.drawing_offset.x + gpu_vertex_position_x(vp);
      vertices[i].y = g.drawing_offset.y + gpu_vertex_position_y(vp);
      vertices[i].texcoord = textured ? (u16)fifo_pop(&g.fifo) : 0;
    }

    if (gpu_trace_disp_enabled()) {
      static u32 poly_n = 0;
      if ((poly_n++ % 1024u) == 0u)
        fprintf(stderr, "[disp poly #%u] off=(%d,%d) area=(%u,%u)-(%u,%u) "
                        "v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) tex=%u\n",
                poly_n,
                (int)g.drawing_offset.x, (int)g.drawing_offset.y,
                (unsigned)g.drawing_area.left, (unsigned)g.drawing_area.top,
                (unsigned)g.drawing_area.right, (unsigned)g.drawing_area.bottom,
                (int)vertices[0].x, (int)vertices[0].y,
                (int)vertices[1].x, (int)vertices[1].y,
                (int)vertices[2].x, (int)vertices[2].y,
                (unsigned)textured);
    }

    gpu_backend_draw_polygon_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    gpu_fill_draw_command(&cmd.base, rc);
    cmd.base.num_vertices = (u16)num_vertices;
    gpu_backend_draw_polygon(&cmd, vertices);
  }

  gpu_end_command();
  return true;
}

static bool gpu_handle_render_rectangle(void)
{
  const gpu_render_command_t rc = { fifo_peek(&g.fifo, 0) };
  const u32 total_words = 2u + (gpu_render_command_texture_enable(rc) ? 1u : 0u)
                            + ((gpu_render_command_rectangle_size(rc) == GPU_DRAW_RECTANGLE_SIZE_VARIABLE) ? 1u : 0u);
  CHECK_COMMAND_SIZE(total_words);

  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  if (gpu_render_command_texture_enable(rc)) {
    gpu_set_texture_palette((u16)(fifo_peek(&g.fifo, 2) >> 16));
    gpu_update_clut_if_needed(gpu_draw_mode_reg_texture_mode(g.draw_mode.mode_reg), g.draw_mode.palette_reg);
  }

  gpu_add_command_ticks(16);
  g.render_command.bits = rc.bits;
  fifo_remove_one(&g.fifo);
  gpu_prepare_for_draw();

  gpu_backend_draw_rectangle_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  gpu_fill_draw_command(&cmd.base, rc);
  cmd.base.num_vertices = 4;
  cmd.color = gpu_render_command_color_for_first_vertex(rc);

  const gpu_vertex_position_t vp = { fifo_pop(&g.fifo) };
  cmd.x = truncate_gpu_vertex_position(g.drawing_offset.x + gpu_vertex_position_x(vp));
  cmd.y = truncate_gpu_vertex_position(g.drawing_offset.y + gpu_vertex_position_y(vp));

  if (gpu_render_command_texture_enable(rc)) {
    const u32 tc_pal = fifo_pop(&g.fifo);
    cmd.base.palette.bits = (u16)(tc_pal >> 16);
    cmd.texcoord = (u16)tc_pal;
  }

  switch (gpu_render_command_rectangle_size(rc)) {
    case GPU_DRAW_RECTANGLE_SIZE_R1X1:   cmd.width = 1;  cmd.height = 1;  break;
    case GPU_DRAW_RECTANGLE_SIZE_R8X8:   cmd.width = 8;  cmd.height = 8;  break;
    case GPU_DRAW_RECTANGLE_SIZE_R16X16: cmd.width = 16; cmd.height = 16; break;
    default: {
      const u32 wh = fifo_pop(&g.fifo);
      cmd.width  = (u16)( wh        & VRAM_WIDTH_MASK);
      cmd.height = (u16)((wh >> 16) & VRAM_HEIGHT_MASK);
      break;
    }
  }
  if (gpu_trace_disp_enabled()) {
    static u32 rect_n = 0;
    if ((rect_n++ % 4096u) == 0u)
      fprintf(stderr, "[disp rect #%u] off=(%d,%d) area=(%u,%u)-(%u,%u) "
                      "pos=(%d,%d) %ux%u tex=%u\n",
              rect_n,
              (int)g.drawing_offset.x, (int)g.drawing_offset.y,
              (unsigned)g.drawing_area.left, (unsigned)g.drawing_area.top,
              (unsigned)g.drawing_area.right, (unsigned)g.drawing_area.bottom,
              (int)cmd.x, (int)cmd.y, (unsigned)cmd.width, (unsigned)cmd.height,
              (unsigned)gpu_render_command_texture_enable(rc));
  }
  gpu_backend_draw_rectangle(&cmd);
  gpu_end_command();
  return true;
}

static bool gpu_handle_render_line(void)
{
  const gpu_render_command_t rc = { fifo_peek(&g.fifo, 0) };
  const u32 total_words = gpu_render_command_shading_enable(rc) ? 4u : 3u;
  CHECK_COMMAND_SIZE(total_words);

  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  g.render_command.bits = rc.bits;
  fifo_remove_one(&g.fifo);
  gpu_prepare_for_draw();

  if (g_settings.gpu_pgxp_enable) {

    gpu_backend_precise_line_vertex_t pv[2];
    memset(pv, 0, sizeof(pv));
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    for (u32 i = 0; i < 2; i++) {
      const u32 color =
          ((i != 0 && gpu_render_command_shading_enable(rc)) ? fifo_pop(&g.fifo) : rc.bits)
          & 0x00FFFFFFu;
      const u64 maddr_and_pos = fifo_pop_u64(&g.fifo);
      const gpu_vertex_position_t vp = { (u32)maddr_and_pos };
      pv[i].native_x = g.drawing_offset.x + gpu_vertex_position_x(vp);
      pv[i].native_y = g.drawing_offset.y + gpu_vertex_position_y(vp);
      pv[i].color = color;
      valid_w &= pgxp_get_precise_vertex((u32)(maddr_and_pos >> 32), vp.bits,
                                          pv[i].native_x, pv[i].native_y,
                                          g.drawing_offset.x, g.drawing_offset.y, 
                                          &pv[i].x, &pv[i].y, &pv[i].w);
    }
    if (!valid_w) {
      pv[0].w = 1.0f;
      pv[1].w = 1.0f;
    }

    /* Cull too-large lines. */
    const s32 minx = (pv[0].native_x < pv[1].native_x) ? pv[0].native_x : pv[1].native_x;
    const s32 maxx = (pv[0].native_x > pv[1].native_x) ? pv[0].native_x : pv[1].native_x;
    const s32 miny = (pv[0].native_y < pv[1].native_y) ? pv[0].native_y : pv[1].native_y;
    const s32 maxy = (pv[0].native_y > pv[1].native_y) ? pv[0].native_y : pv[1].native_y;
    if ((maxx + 1 - minx) > (s32)MAX_PRIMITIVE_WIDTH ||
        (maxy + 1 - miny) > (s32)MAX_PRIMITIVE_HEIGHT) {
      gpu_add_command_ticks(16);
      gpu_end_command();
      return true;
    }

    gpu_backend_draw_precise_line_cmd_t pcmd;
    memset(&pcmd, 0, sizeof(pcmd));
    gpu_fill_draw_command(&pcmd.base, rc);
    pcmd.base.palette.bits = 0;
    pcmd.base.num_vertices = 2;
    pcmd.base.valid_w = valid_w;
    gpu_add_command_ticks(16);
    gpu_backend_draw_precise_line(&pcmd, pv);
    gpu_end_command();
    return true;
  }

  gpu_backend_draw_line_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  gpu_fill_draw_command(&cmd.base, rc);
  cmd.base.palette.bits = 0;
  cmd.base.num_vertices = 2;

  gpu_backend_line_vertex_t vertices[2];
  memset(vertices, 0, sizeof(vertices));
  if (gpu_render_command_shading_enable(rc)) {
    vertices[0].color = gpu_render_command_color_for_first_vertex(rc);
    const gpu_vertex_position_t s = { fifo_pop(&g.fifo) };
    vertices[0].x = g.drawing_offset.x + gpu_vertex_position_x(s);
    vertices[0].y = g.drawing_offset.y + gpu_vertex_position_y(s);
    vertices[1].color = fifo_pop(&g.fifo) & 0x00FFFFFFu;
    const gpu_vertex_position_t e = { fifo_pop(&g.fifo) };
    vertices[1].x = g.drawing_offset.x + gpu_vertex_position_x(e);
    vertices[1].y = g.drawing_offset.y + gpu_vertex_position_y(e);
  } else {
    vertices[0].color = gpu_render_command_color_for_first_vertex(rc);
    vertices[1].color = vertices[0].color;
    const gpu_vertex_position_t s = { fifo_pop(&g.fifo) };
    vertices[0].x = g.drawing_offset.x + gpu_vertex_position_x(s);
    vertices[0].y = g.drawing_offset.y + gpu_vertex_position_y(s);
    const gpu_vertex_position_t e = { fifo_pop(&g.fifo) };
    vertices[1].x = g.drawing_offset.x + gpu_vertex_position_x(e);
    vertices[1].y = g.drawing_offset.y + gpu_vertex_position_y(e);
  }
  gpu_add_command_ticks(16);
  gpu_backend_draw_line(&cmd, vertices);
  gpu_end_command();
  return true;
}

static bool gpu_handle_render_polyline(void)
{
  const gpu_render_command_t rc = { fifo_peek(&g.fifo, 0) };
  const u32 min_words = gpu_render_command_shading_enable(rc) ? 3u : 4u;
  CHECK_COMMAND_SIZE(min_words);

  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  gpu_add_command_ticks(16);
  g.render_command.bits = rc.bits;
  fifo_remove_one(&g.fifo);

  /* Pop the first vertices then switch state to look for the terminator. */
  const u32 to_pop = min_words - 1;
  for (u32 i = 0; i < to_pop && g.polyline_buffer.size < GPU_MAX_POLYLINE_WORDS; i++)
    g.polyline_buffer.data[g.polyline_buffer.size++] = fifo_pop_u64(&g.fifo);

  g.blitter_state = GPU_BLITTER_DRAWING_POLYLINE;
  g.command_total_words = 0;
  return true;
}

static void gpu_finish_polyline(void)
{
  gpu_prepare_for_draw();
  const bool shaded = gpu_render_command_shading_enable(g.render_command);
  const u32 num_vertices =
    ((u32)g.polyline_buffer.size + (shaded ? 1u : 0u)) >> (shaded ? 1u : 0u);
  if (num_vertices < 2) return;

  enum { MAX_OUT = 1024 };

  if (g_settings.gpu_pgxp_enable) {

    gpu_backend_precise_line_vertex_t out[MAX_OUT];
    u32 out_count = 0;
    u32 buffer_pos = 0;
    bool valid_w = g_settings.gpu_pgxp_texture_correction;
    gpu_backend_precise_line_vertex_t start, end;
    memset(&start, 0, sizeof(start));
    memset(&end,   0, sizeof(end));

    /* Read first vertex (color from render command, full u64 from buffer). */
    {
      const u64 maddr_and_pos = g.polyline_buffer.data[buffer_pos++];
      const gpu_vertex_position_t vp = { (u32)maddr_and_pos };
      start.color = gpu_render_command_color_for_first_vertex(g.render_command);
      start.native_x = g.drawing_offset.x + gpu_vertex_position_x(vp);
      start.native_y = g.drawing_offset.y + gpu_vertex_position_y(vp);
      valid_w &= pgxp_get_precise_vertex((u32)(maddr_and_pos >> 32), vp.bits,
                                          start.native_x, start.native_y,
                                          g.drawing_offset.x, g.drawing_offset.y, 
                                          &start.x, &start.y, &start.w);
    }

    for (u32 i = 1; i < num_vertices && out_count + 1 < MAX_OUT; i++) {
      const u32 color = shaded
          ? ((u32)g.polyline_buffer.data[buffer_pos++] & 0x00FFFFFFu)
          : (g.render_command.bits & 0x00FFFFFFu);
      const u64 maddr_and_pos = g.polyline_buffer.data[buffer_pos++];
      const gpu_vertex_position_t vp = { (u32)maddr_and_pos };
      end.color = color;
      end.native_x = g.drawing_offset.x + gpu_vertex_position_x(vp);
      end.native_y = g.drawing_offset.y + gpu_vertex_position_y(vp);
      valid_w &= pgxp_get_precise_vertex((u32)(maddr_and_pos >> 32), vp.bits,
                                          end.native_x, end.native_y,
                                          g.drawing_offset.x, g.drawing_offset.y, 
                                          &end.x, &end.y, &end.w);

      const s32 minx = (start.native_x < end.native_x) ? start.native_x : end.native_x;
      const s32 maxx = (start.native_x > end.native_x) ? start.native_x : end.native_x;
      const s32 miny = (start.native_y < end.native_y) ? start.native_y : end.native_y;
      const s32 maxy = (start.native_y > end.native_y) ? start.native_y : end.native_y;
      const bool too_big = ((maxx + 1 - minx) > (s32)MAX_PRIMITIVE_WIDTH) ||
                           ((maxy + 1 - miny) > (s32)MAX_PRIMITIVE_HEIGHT);
      if (!too_big) {
        out[out_count++] = start;
        out[out_count++] = end;
      }
      start = end;
    }

    if (out_count > 0) {
      if (!valid_w) {
        for (u32 i = 0; i < out_count; i++) out[i].w = 1.0f;
      }
      gpu_backend_draw_precise_line_cmd_t pcmd;
      memset(&pcmd, 0, sizeof(pcmd));
      gpu_fill_draw_command(&pcmd.base, g.render_command);
      pcmd.base.palette.bits = 0;
      pcmd.base.num_vertices = (u16)out_count;
      pcmd.base.valid_w = valid_w;
      gpu_backend_draw_precise_line(&pcmd, out);
    }
    return;
  }

  gpu_backend_draw_line_cmd_t cmd;
  memset(&cmd, 0, sizeof(cmd));
  gpu_fill_draw_command(&cmd.base, g.render_command);
  cmd.base.palette.bits = 0;

  u32 buffer_pos = 0;
  gpu_backend_line_vertex_t out[MAX_OUT];
  u32 out_count = 0;

  s32 cur_x, cur_y; u32 cur_color;
  cur_color = gpu_render_command_color_for_first_vertex(g.render_command);
  const gpu_vertex_position_t vp0 = { (u32)g.polyline_buffer.data[buffer_pos++] };
  cur_x = g.drawing_offset.x + gpu_vertex_position_x(vp0);
  cur_y = g.drawing_offset.y + gpu_vertex_position_y(vp0);

  for (u32 i = 1; i < num_vertices && out_count + 1 < MAX_OUT; i++) {
    u32 end_color = shaded ? ((u32)g.polyline_buffer.data[buffer_pos++] & 0x00FFFFFFu)
                           : gpu_render_command_color_for_first_vertex(g.render_command);
    const gpu_vertex_position_t vp = { (u32)g.polyline_buffer.data[buffer_pos++] };
    const s32 ex = g.drawing_offset.x + gpu_vertex_position_x(vp);
    const s32 ey = g.drawing_offset.y + gpu_vertex_position_y(vp);

    out[out_count].x = cur_x; out[out_count].y = cur_y; out[out_count].color = cur_color; out_count++;
    out[out_count].x = ex;    out[out_count].y = ey;    out[out_count].color = end_color; out_count++;

    cur_x = ex; cur_y = ey; cur_color = end_color;
  }

  if (out_count > 0) {
    cmd.base.num_vertices = (u16)out_count;
    gpu_backend_draw_line(&cmd, out);
  }
}

static bool gpu_handle_fill_rectangle(void)
{
  CHECK_COMMAND_SIZE(3);
  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  const u32 color = fifo_pop(&g.fifo) & 0x00FFFFFFu;
  const u32 cw = fifo_peek(&g.fifo, 0);
  const u32 dst_x = cw & 0x3F0u;
  const u32 dst_y = (fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK;
  const u32 sw = fifo_peek(&g.fifo, 0);
  const u32 width  = ((sw & VRAM_WIDTH_MASK) + 0xFu) & ~0xFu;
  const u32 height = (fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK;

  if (width > 0 && height > 0) {
    gpu_backend_fill_vram_cmd_t c;
    c.x = (u16)dst_x; c.y = (u16)dst_y;
    c.width = (u16)width; c.height = (u16)height;
    c.color = color;
    c.interlaced_rendering = gpu_is_interlaced_rendering_enabled();
    c.active_line_lsb = g.crtc_state.active_line_lsb;
    gpu_backend_fill_vram(&c);
  }
  gpu_add_command_ticks(46 + ((width / 8) + 9) * height);
  gpu_end_command();
  return true;
}

static bool gpu_handle_copy_cpu_to_vram(void)
{
  CHECK_COMMAND_SIZE(3);
  fifo_remove_one(&g.fifo);
  const u32 coords = fifo_pop(&g.fifo);
  const u32 size   = fifo_pop(&g.fifo);

  /* Tenga Seiha sends nonsense writes on boot; ignore the obvious ones. */
  if (size == 0xFFFFFFFFu) {
    ERROR_LOG("Ignoring likely-invalid VRAM write to (%u,%u)",
              coords & VRAM_WIDTH_MASK, (coords >> 16) & VRAM_HEIGHT_MASK);
    return true;
  }

  const u32 dst_x = coords & VRAM_WIDTH_MASK;
  const u32 dst_y = (coords >> 16) & VRAM_HEIGHT_MASK;
  const u32 cw    = replace_zero(size & VRAM_WIDTH_MASK, 0x400u);
  const u32 ch    = replace_zero((size >> 16) & VRAM_HEIGHT_MASK, 0x200u);
  const u32 num_pixels = cw * ch;
  const u32 num_words  = (num_pixels + 1u) / 2u;

  gpu_end_command();

  g.blitter_state = GPU_BLITTER_WRITING_VRAM;
  g.blit_buffer.size = 0;
  gpu_grow_blit_buf(num_words);
  g.blit_remaining_words = num_words;
  g.vram_transfer.x = (u16)dst_x;
  g.vram_transfer.y = (u16)dst_y;
  g.vram_transfer.width = (u16)cw;
  g.vram_transfer.height = (u16)ch;
  return true;
}

static void gpu_finish_vram_write(void)
{
  if (gpu_is_interlaced_rendering_enabled() && gpu_is_crtc_scanline_pending())
    gpu_synchronize_crtc();

  {
    const u32 hdr[6] = { g.vram_transfer.x, g.vram_transfer.y,
                         g.vram_transfer.width, g.vram_transfer.height,
                         g.blit_buffer.size, g.blit_remaining_words };
    FMV_DUMP("T6_hdr", hdr, sizeof(hdr),
             ((u64)g.vram_transfer.y << 16) | g.vram_transfer.x);
    FMV_DUMP("T6", g.blit_buffer.data, g.blit_buffer.size * 4u,
             ((u64)g.vram_transfer.y << 16) | g.vram_transfer.x);
  }

  if (g.blit_remaining_words == 0) {
    gpu_update_vram(g.vram_transfer.x, g.vram_transfer.y, g.vram_transfer.width, g.vram_transfer.height,
                    g.blit_buffer.data, gpustat_set_mask_while_drawing(g.GPUSTAT),
                    gpustat_check_mask_before_draw(g.GPUSTAT));
  } else {
    /* Partial transfer: write whole rows + a partial last row. */
    const u32 num_pixels = (u32)g.vram_transfer.width * g.vram_transfer.height;
    const u32 num_words = (num_pixels + 1u) / 2u;
    const u32 transferred_words = num_words - g.blit_remaining_words;
    const u32 transferred_pixels = transferred_words * 2u;
    const u32 transferred_full_rows = transferred_pixels / g.vram_transfer.width;
    const u32 transferred_width_last_row = transferred_pixels % g.vram_transfer.width;
    const u8* blit_ptr = (const u8*)g.blit_buffer.data;
    if (transferred_full_rows > 0) {
      gpu_update_vram(g.vram_transfer.x, g.vram_transfer.y, g.vram_transfer.width, (u16)transferred_full_rows,
                      blit_ptr, gpustat_set_mask_while_drawing(g.GPUSTAT), gpustat_check_mask_before_draw(g.GPUSTAT));
      blit_ptr += (u32)g.vram_transfer.width * transferred_full_rows * sizeof(u16);
    }
    if (transferred_width_last_row > 0) {
      gpu_update_vram(g.vram_transfer.x, (u16)(g.vram_transfer.y + transferred_full_rows),
                      (u16)transferred_width_last_row, 1, blit_ptr,
                      gpustat_set_mask_while_drawing(g.GPUSTAT), gpustat_check_mask_before_draw(g.GPUSTAT));
    }
  }
  g.blit_buffer.size = 0;
  memset(&g.vram_transfer, 0, sizeof(g.vram_transfer));
  g.blitter_state = GPU_BLITTER_IDLE;
}

static bool gpu_handle_copy_vram_to_cpu(void)
{
  CHECK_COMMAND_SIZE(3);
  fifo_remove_one(&g.fifo);

  g.vram_transfer.x = (u16)(fifo_peek(&g.fifo, 0) & VRAM_WIDTH_MASK);
  g.vram_transfer.y = (u16)((fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK);
  g.vram_transfer.width  = (u16)((((u16)fifo_peek(&g.fifo, 0)) - 1u) & VRAM_WIDTH_MASK) + 1u;
  g.vram_transfer.height = (u16)((((u16)(fifo_pop(&g.fifo) >> 16)) - 1u) & VRAM_HEIGHT_MASK) + 1u;
  gpu_read_vram(g.vram_transfer.x, g.vram_transfer.y, g.vram_transfer.width, g.vram_transfer.height);
  g.blitter_state = GPU_BLITTER_READING_VRAM;
  g.command_total_words = 0;
  return true;
}

static bool gpu_handle_copy_vram_to_vram(void)
{
  CHECK_COMMAND_SIZE(4);
  fifo_remove_one(&g.fifo);
  const u32 src_x = fifo_peek(&g.fifo, 0) & VRAM_WIDTH_MASK;
  const u32 src_y = (fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK;
  const u32 dst_x = fifo_peek(&g.fifo, 0) & VRAM_WIDTH_MASK;
  const u32 dst_y = (fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK;
  const u32 width  = replace_zero(fifo_peek(&g.fifo, 0) & VRAM_WIDTH_MASK, 0x400u);
  const u32 height = replace_zero((fifo_pop(&g.fifo) >> 16) & VRAM_HEIGHT_MASK, 0x200u);

  /* Most "no-op" copies are 2x2 same-coords end-of-frame copies; skip them. */
  const bool skip = width == 0 || height == 0 ||
                    (src_x == dst_x && src_y == dst_y && !gpustat_set_mask_while_drawing(g.GPUSTAT));
  if (!skip) {
    gpu_backend_copy_vram_cmd_t c;
    c.src_x = (u16)src_x; c.src_y = (u16)src_y;
    c.dst_x = (u16)dst_x; c.dst_y = (u16)dst_y;
    c.width = (u16)width; c.height = (u16)height;
    c.set_mask_while_drawing = gpustat_set_mask_while_drawing(g.GPUSTAT);
    c.check_mask_before_draw = gpustat_check_mask_before_draw(g.GPUSTAT);
    gpu_backend_copy_vram(&c);
  }
  gpu_add_command_ticks(width * height * 2u);
  gpu_end_command();
  return true;
}

/* GP1 ringbuffer for SIGINT dump. */
typedef struct { u64 tick; u32 word; } gpu_dbg_gp1_entry_t;
#define GPU_DBG_GP1_LOG_SIZE 64u
static gpu_dbg_gp1_entry_t s_dbg_gp1_log[GPU_DBG_GP1_LOG_SIZE];
static u32 s_dbg_gp1_log_pos;
static u32 s_dbg_gp1_log_count;

static const char* gp1_cmd_name(u32 cmd) {
  switch (cmd & 0x3Fu) {
    case 0x00: return "ResetGPU";
    case 0x01: return "ClearFIFO";
    case 0x02: return "AckIRQ";
    case 0x03: return "DisplayEn";
    case 0x04: return "DMADir";
    case 0x05: return "DispVRAMStart";
    case 0x06: return "HRange";
    case 0x07: return "VRange";
    case 0x08: return "DispMode";
    case 0x09: return "TexDis";
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F: return "GetGPUInfo";
    default:   return "?";
  }
}

__attribute__((visibility("default")))
void gpu_dbg_dump_recent_gp1(FILE* out)
{
  if (!out) out = stderr;
  fprintf(out, "===== last %u GP1 writes (oldest first) =====\n", s_dbg_gp1_log_count);
  const u32 n = s_dbg_gp1_log_count;
  const u32 start = (n < GPU_DBG_GP1_LOG_SIZE) ? 0u
                    : (s_dbg_gp1_log_pos % GPU_DBG_GP1_LOG_SIZE);
  for (u32 i = 0; i < n; i++) {
    const gpu_dbg_gp1_entry_t* e = &s_dbg_gp1_log[(start + i) % GPU_DBG_GP1_LOG_SIZE];
    const u32 cmd = (e->word >> 24) & 0x3Fu;
    fprintf(out, "  [%2u] tick=%llu  GP1 0x%08X  cmd=0x%02X (%s) param=0x%06X\n",
            i, (unsigned long long)e->tick, e->word, cmd, gp1_cmd_name(cmd), e->word & 0x00FFFFFFu);
  }
  fprintf(out, "  current GPUSTAT=0x%08X\n", (unsigned)g.GPUSTAT.bits);
}

/* GP0 ringbuffer + optional file trace.  Per-instruction MMIO/DMA ordering
 * of GP0 writes is a class of divergence the per-block register diff
 * cannot see.  Captures every GP0 word with a tick stamp, source tag
 * (mmio or dma), and frame number so an interp run and a recomp run can
 * be diffed at the GPU-stream level.  gpu_dbg_gp0_src_t and the static
 * fwd-decl for gpu_dbg_log_gp0 live in gpu.c so the early call sites can
 * see them. */

typedef struct {
  u64 tick;
  u32 frame;
  u32 word;
  u32 source;
} gpu_dbg_gp0_entry_t;

#define GPU_DBG_GP0_LOG_SIZE 64u
static gpu_dbg_gp0_entry_t s_dbg_gp0_log[GPU_DBG_GP0_LOG_SIZE];
static u32 s_dbg_gp0_log_pos;
static u32 s_dbg_gp0_log_count;

static FILE* s_gp0_trace_file       = NULL;
static int   s_gp0_trace_evaluated  = 0;  /* 0=untried, 1=opened, 2=disabled */

static void gp0_trace_open_lazy(void)
{
  if (s_gp0_trace_evaluated) return;
  const char* path = getenv("CUPID_TRACE_GP0");
  if (path == NULL || path[0] == '\0') {
    s_gp0_trace_evaluated = 2;
    return;
  }
  s_gp0_trace_file = fopen(path, "w");
  s_gp0_trace_evaluated = (s_gp0_trace_file != NULL) ? 1 : 2;
  if (s_gp0_trace_file != NULL)
    setvbuf(s_gp0_trace_file, NULL, _IOLBF, 0);
}

static const char* gp0_cmd_short(u32 word)
{
  const u32 op = (word >> 24) & 0xFFu;
  if (op == 0x00) return "nop";
  if (op == 0x01) return "clr-cache";
  if (op == 0x02) return "fill-rect";
  if (op == 0x1F) return "irq-req";
  if (op >= 0x20 && op <= 0x3F) return "render-poly";
  if (op >= 0x40 && op <= 0x5F) return "render-line";
  if (op >= 0x60 && op <= 0x7F) return "render-rect";
  if (op >= 0x80 && op <= 0x9F) return "vram-vram";
  if (op >= 0xA0 && op <= 0xBF) return "cpu-vram";
  if (op >= 0xC0 && op <= 0xDF) return "vram-cpu";
  if (op == 0xE1) return "draw-mode";
  if (op == 0xE2) return "tex-window";
  if (op == 0xE3) return "draw-tl";
  if (op == 0xE4) return "draw-br";
  if (op == 0xE5) return "draw-off";
  if (op == 0xE6) return "mask-bit";
  return "data";
}

static void gpu_dbg_log_gp0(u32 word, gpu_dbg_gp0_src_t source)
{
  /* Ringbuffer (always on, cheap). */
  gpu_dbg_gp0_entry_t* e = &s_dbg_gp0_log[s_dbg_gp0_log_pos];
  const u64 tick = (u64)timing_events_get_global_tick_counter();
  const u32 frame = system_get_frame_number ? system_get_frame_number() : 0u;
  e->tick   = tick;
  e->frame  = frame;
  e->word   = word;
  e->source = (u32)source;
  s_dbg_gp0_log_pos = (s_dbg_gp0_log_pos + 1u) % GPU_DBG_GP0_LOG_SIZE;
  if (s_dbg_gp0_log_count < GPU_DBG_GP0_LOG_SIZE) s_dbg_gp0_log_count++;

  /* Optional file trace (env-gated). */
  gp0_trace_open_lazy();
  if (s_gp0_trace_file != NULL) {
    fprintf(s_gp0_trace_file,
            "frame=%u tick=%llu src=%s word=0x%08X op=0x%02X (%s)\n",
            frame, (unsigned long long)tick,
            (source == GPU_DBG_GP0_SRC_MMIO) ? "mmio" : "dma",
            word, (word >> 24) & 0xFFu, gp0_cmd_short(word));
  }
}

__attribute__((visibility("default")))
void gpu_dbg_dump_recent_gp0(FILE* out)
{
  if (!out) out = stderr;
  fprintf(out, "===== last %u GP0 writes (oldest first) =====\n", s_dbg_gp0_log_count);
  const u32 n = s_dbg_gp0_log_count;
  const u32 start = (n < GPU_DBG_GP0_LOG_SIZE) ? 0u
                    : (s_dbg_gp0_log_pos % GPU_DBG_GP0_LOG_SIZE);
  for (u32 i = 0; i < n; i++) {
    const gpu_dbg_gp0_entry_t* e = &s_dbg_gp0_log[(start + i) % GPU_DBG_GP0_LOG_SIZE];
    fprintf(out,
            "  [%2u] frame=%u tick=%llu src=%s GP0 0x%08X op=0x%02X (%s)\n",
            i, e->frame, (unsigned long long)e->tick,
            (e->source == GPU_DBG_GP0_SRC_MMIO) ? "mmio" : "dma ",
            e->word, (e->word >> 24) & 0xFFu, gp0_cmd_short(e->word));
  }
}

static void gpu_write_gp1(u32 value)
{
  {
    gpu_dbg_gp1_entry_t* e = &s_dbg_gp1_log[s_dbg_gp1_log_pos];
    e->tick = (u64)timing_events_get_global_tick_counter();
    e->word = value;
    s_dbg_gp1_log_pos = (s_dbg_gp1_log_pos + 1u) % GPU_DBG_GP1_LOG_SIZE;
    if (s_dbg_gp1_log_count < GPU_DBG_GP1_LOG_SIZE) s_dbg_gp1_log_count++;
  }
  const u32 command = (value >> 24) & 0x3Fu;
  const u32 param   = value & 0x00FFFFFFu;
  switch (command) {
    case GP1_COMMAND_RESET_GPU:
      timing_event_invoke_early(&g.command_tick_event, false);
      gpu_synchronize_crtc();
      gpu_soft_reset();
      break;
    case GP1_COMMAND_CLEAR_FIFO:
      timing_event_invoke_early(&g.command_tick_event, false);
      gpu_synchronize_crtc();
      if (g.blitter_state == GPU_BLITTER_WRITING_VRAM) gpu_finish_vram_write();
      g.blitter_state = GPU_BLITTER_IDLE;
      g.command_total_words = 0;
      memset(&g.vram_transfer, 0, sizeof(g.vram_transfer));
      fifo_clear(&g.fifo);
      g.blit_buffer.size = 0;
      g.blit_remaining_words = 0;
      g.pending_command_ticks = 0;
      timing_event_deactivate(&g.command_tick_event);
      gpu_update_dma_request();
      gpu_update_gpu_idle();
      break;
    case GP1_COMMAND_ACKNOWLEDGE_INTERRUPT:
      g.GPUSTAT.bits &= ~(1u << 24);
      interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_GPU, false);
      break;
    case GP1_COMMAND_SET_DISPLAY_DISABLE: {
      const bool disable = (value & 0x1) != 0;
      gpu_synchronize_crtc();
      if (disable) g.GPUSTAT.bits |= (1u << 23); else g.GPUSTAT.bits &= ~(1u << 23);
      break;
    }
    case GP1_COMMAND_SET_DMA_DIRECTION: {
      const gpu_dma_direction_t newd = (gpu_dma_direction_t)(param & 0x3u);
      if (gpustat_dma_direction(g.GPUSTAT) != newd) {
        g.GPUSTAT.bits = (g.GPUSTAT.bits & ~(3u << 29)) | (((u32)newd & 0x3u) << 29);
        gpu_update_dma_request();
      }
      break;
    }
    case GP1_COMMAND_SET_DISPLAY_START_ADDRESS: {
      const u32 v = param & DISPLAY_ADDRESS_START_MASK;
      if (gpu_trace_disp_enabled()) {
        static u32 swap_n = 0;
        fprintf(stderr, "[disp swap #%u] GP1.05 prev=0x%05X new=0x%05X scanline=%u\n",
                swap_n++,
                (unsigned)g.crtc_state.display_address_start,
                (unsigned)v,
                (unsigned)g.crtc_state.current_scanline);
      }
      if (system_increment_internal_frame_number) system_increment_internal_frame_number();
      if (g.crtc_state.display_address_start != v) {
        gpu_synchronize_crtc();
        g.crtc_state.display_address_start = v;
        gpu_update_crtc_display_parameters();
      }
      break;
    }
    case GP1_COMMAND_SET_HORIZONTAL_DISPLAY_RANGE: {
      const u32 v = param & HORIZONTAL_DISPLAY_RANGE_MASK;
      if (g.crtc_state.horizontal_display_range != v) {
        gpu_synchronize_crtc();
        g.crtc_state.horizontal_display_range = v;
        gpu_update_crtc_config();
      }
      break;
    }
    case GP1_COMMAND_SET_VERTICAL_DISPLAY_RANGE: {
      const u32 v = param & VERTICAL_DISPLAY_RANGE_MASK;
      if (g.crtc_state.vertical_display_range != v) {
        gpu_synchronize_crtc();
        g.crtc_state.vertical_display_range = v;
        gpu_update_crtc_config();
      }
      break;
    }
    case GP1_COMMAND_SET_DISPLAY_MODE: {
      const gp1_set_display_mode_t dm = { param };
      u32 newst = g.GPUSTAT.bits;
      newst = (newst & ~(3u << 17)) | (((u32)gp1_set_display_mode_horizontal_resolution_1(dm) & 0x3u) << 17);
      if (gp1_set_display_mode_vertical_resolution(dm))     newst |= (1u << 19); else newst &= ~(1u << 19);
      if (gp1_set_display_mode_pal_mode(dm))                newst |= (1u << 20); else newst &= ~(1u << 20);
      if (gp1_set_display_mode_display_area_color_depth(dm))newst |= (1u << 21); else newst &= ~(1u << 21);
      if (gp1_set_display_mode_vertical_interlace(dm))      newst |= (1u << 22); else newst &= ~(1u << 22);
      if (gp1_set_display_mode_horizontal_resolution_2(dm)) newst |= (1u << 16); else newst &= ~(1u << 16);
      if (gp1_set_display_mode_reverse_flag(dm))            newst |= (1u << 14); else newst &= ~(1u << 14);
      if (g.GPUSTAT.bits != newst) {
        const u32 SET_MASK = 0x007F4000u;
        timing_event_invoke_early(&g.command_tick_event, false);
        gpu_synchronize_crtc();
        g.GPUSTAT.bits = (g.GPUSTAT.bits & ~SET_MASK) | (newst & SET_MASK);
        gpu_update_crtc_config();
      }
      break;
    }
    case GP1_COMMAND_SET_ALLOW_TEXTURE_DISABLE:
      g.set_texture_disable_mask = (param & 0x1u) != 0;
      break;
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
      gpu_handle_get_gpu_info_command(value);
      break;
    default:
      ERROR_LOG("Unimplemented GP1 command 0x%02X", command);
      break;
  }
}

static void gpu_handle_get_gpu_info_command(u32 value)
{
  const u8 sub = (u8)(value & 0x07u);
  switch (sub) {
    case 0x00: case 0x01: case 0x06: case 0x07:
      break; /* leave latch */
    case 0x02: g.GPUREAD_latch = g.draw_mode.texture_window_value; break;
    case 0x03: g.GPUREAD_latch = g.drawing_area.left | (g.drawing_area.top << 10); break;
    case 0x04: g.GPUREAD_latch = g.drawing_area.right | (g.drawing_area.bottom << 10); break;
    case 0x05: g.GPUREAD_latch = ((u32)g.drawing_offset.x & 0x7FFu) | (((u32)g.drawing_offset.y & 0x7FFu) << 11); break;
    default:   WARNING_LOG("Unhandled GetGPUInfo(0x%02X)", sub); break;
  }
}
