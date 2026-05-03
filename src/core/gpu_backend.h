/*
 * virtuals (DrawPolygon, FillVRAM, ...).  Here the back-end is a struct + a
 * vtable of function pointers; a concrete back-end (gpu_sw_t) embeds a
 * gpu_backend_t and points the vtable at its own static functions.
 *
 * The hardware-renderer command queue / video-thread split is dropped; the
 * software path runs synchronously on the CPU thread, so command "structs"
 * here are passed in by value (or by stack pointer) directly to the vtable
 * methods rather than being marshalled through a ring buffer.
 *
 * Hardware-renderer-only command types (Reconfigure, Submit, BufferSwapped,
 * UpdateGameInfo, AsyncCall, ...) are dropped entirely.
 */

#ifndef CUPID_CORE_GPU_BACKEND_H
#define CUPID_CORE_GPU_BACKEND_H

#include "core/gpu_types.h"
#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;

typedef struct {
  u16 x, y;
  u16 width, height;
} gpu_backend_read_vram_cmd_t;

typedef struct {
  u16 x, y;
  u16 width, height;
  u32 color;
  bool interlaced_rendering;
  u8   active_line_lsb;
} gpu_backend_fill_vram_cmd_t;

typedef struct {
  u16 x, y;
  u16 width, height;
  bool set_mask_while_drawing;
  bool check_mask_before_draw;
  /* data is appended by caller as a separate u16* parameter. */
} gpu_backend_update_vram_cmd_t;

typedef struct {
  u16 src_x, src_y;
  u16 dst_x, dst_y;
  u16 width, height;
  bool set_mask_while_drawing;
  bool check_mask_before_draw;
} gpu_backend_copy_vram_cmd_t;

typedef struct {
  gpu_drawing_area_t new_area;
} gpu_backend_set_drawing_area_cmd_t;

typedef struct {
  gpu_texture_palette_reg_t reg;
  bool clut_is_8bit;
} gpu_backend_update_clut_cmd_t;

/* Common header for all draw commands.  Embedded in polygon/rect/line cmds. */
typedef struct {
  /* Bit-packed flags; union with raw bits for compact passing. */
  bool interlaced_rendering   : 1;
  bool active_line_lsb        : 1; /* 0 = even-line displayed in VRAM, 1 = odd */
  bool set_mask_while_drawing : 1;
  bool check_mask_before_draw : 1;
  bool texture_enable         : 1;
  bool raw_texture_enable     : 1;
  bool transparency_enable    : 1;
  bool shading_enable         : 1;
  bool quad_polygon           : 1;
  bool dither_enable          : 1;
  bool valid_w                : 1; /* meaningful only on precise polygon/line cmds */

  u16  num_vertices;
  gpu_draw_mode_reg_t      draw_mode;
  gpu_texture_palette_reg_t palette;
  gpu_texture_window_t     window;
} gpu_backend_draw_cmd_t;

/* During transfer/render: if ((dst_pixel & mask_and) == 0) pixel = src | mask_or. */
ALWAYS_INLINE u16 gpu_backend_draw_cmd_mask_and(const gpu_backend_draw_cmd_t* c)
{
  return c->check_mask_before_draw ? (u16)0x8000u : (u16)0x0000u;
}
ALWAYS_INLINE u16 gpu_backend_draw_cmd_mask_or(const gpu_backend_draw_cmd_t* c)
{
  return c->set_mask_while_drawing ? (u16)0x8000u : (u16)0x0000u;
}

typedef struct {
  s32 x, y;
  union {
    struct { u8 r, g, b, a; };
    u32 color;
  };
  union {
    struct { u8 u, v; };
    u16 texcoord;
  };
} gpu_backend_polygon_vertex_t;

typedef struct {
  gpu_backend_draw_cmd_t base;
  /* vertices follow as separate ptr+count param. */
} gpu_backend_draw_polygon_cmd_t;

typedef struct {
  gpu_backend_draw_cmd_t base;
  u16 width, height;
  u16 texcoord;
  s32 x, y;
  u32 color;
} gpu_backend_draw_rectangle_cmd_t;

typedef struct {
  s32 x, y;
  union {
    struct { u8 r, g, b, a; };
    u32 color;
  };
} gpu_backend_line_vertex_t;

typedef struct {
  gpu_backend_draw_cmd_t base;
  /* vertices follow as separate ptr+count param. */
} gpu_backend_draw_line_cmd_t;

/* PGXP precise variants; floats carried alongside integer (native_x,
 * native_y) so SW backends can ignore floats and HW backends can use them
 * for sub-pixel transform. */
typedef struct {
  float x, y, w;
  s32   native_x, native_y;
  union {
    struct { u8 r, g, b, a; };
    u32 color;
  };
  union {
    struct { u8 u, v; };
    u16 texcoord;
  };
} gpu_backend_precise_polygon_vertex_t;

typedef struct {
  gpu_backend_draw_cmd_t base;
  /* vertices follow as separate ptr+count param. */
} gpu_backend_draw_precise_polygon_cmd_t;

typedef struct {
  float x, y, w;
  s32   native_x, native_y;
  union {
    struct { u8 r, g, b, a; };
    u32 color;
  };
} gpu_backend_precise_line_vertex_t;

typedef struct {
  gpu_backend_draw_cmd_t base;
  /* vertices follow as separate ptr+count param. */
} gpu_backend_draw_precise_line_cmd_t;

/* Display update parameters; pushed to the back-end at vblank. */
typedef struct {
  u16 display_width;
  u16 display_height;
  u16 display_origin_left;
  u16 display_origin_top;
  u16 display_vram_left;
  u16 display_vram_top;
  u16 display_vram_width;
  u16 display_vram_height;
  float display_pixel_aspect_ratio;
  u8   gpu_busy_pct;
  bool interlaced_display_enabled : 1;
  bool interlaced_display_field   : 1;
  bool interlaced_display_interleaved : 1;
  bool interleaved_480i_mode      : 1;
  bool display_24bit              : 1;
  bool display_disabled           : 1;
  bool submit_frame               : 1;
} gpu_backend_update_display_cmd_t;

typedef struct gpu_backend gpu_backend_t;

typedef struct {
  void (*read_vram) (gpu_backend_t* self, const gpu_backend_read_vram_cmd_t* c);
  void (*fill_vram) (gpu_backend_t* self, const gpu_backend_fill_vram_cmd_t* c);
  void (*update_vram)(gpu_backend_t* self, const gpu_backend_update_vram_cmd_t* c, const u16* data);
  void (*copy_vram) (gpu_backend_t* self, const gpu_backend_copy_vram_cmd_t* c);

  void (*draw_polygon)  (gpu_backend_t* self, const gpu_backend_draw_polygon_cmd_t* c,
                         const gpu_backend_polygon_vertex_t* vertices);
  void (*draw_rectangle)(gpu_backend_t* self, const gpu_backend_draw_rectangle_cmd_t* c);
  void (*draw_line)     (gpu_backend_t* self, const gpu_backend_draw_line_cmd_t* c,
                         const gpu_backend_line_vertex_t* vertices);
  void (*draw_precise_polygon)(gpu_backend_t* self,
                               const gpu_backend_draw_precise_polygon_cmd_t* c,
                               const gpu_backend_precise_polygon_vertex_t* vertices);
  void (*draw_precise_line)   (gpu_backend_t* self,
                               const gpu_backend_draw_precise_line_cmd_t* c,
                               const gpu_backend_precise_line_vertex_t* vertices);

  void (*drawing_area_changed)(gpu_backend_t* self, const gpu_backend_set_drawing_area_cmd_t* c);
  void (*update_clut)         (gpu_backend_t* self, const gpu_backend_update_clut_cmd_t* c);
  void (*clear_cache)         (gpu_backend_t* self);
  void (*clear_vram)          (gpu_backend_t* self);
  void (*update_display)      (gpu_backend_t* self, const gpu_backend_update_display_cmd_t* c);
  void (*flush_render)        (gpu_backend_t* self);

  bool (*do_state)(gpu_backend_t* self, state_wrapper_t* sw);

  void (*destroy)(gpu_backend_t* self);
} gpu_backend_vtable_t;

/* Statistics counters. */
typedef struct {
  u32 num_reads;
  u32 num_writes;
  u32 num_copies;
  u32 num_vertices;
  u32 num_primitives;
} gpu_backend_counters_t;

struct gpu_backend {
  const gpu_backend_vtable_t* vtable;
  gpu_drawing_area_t          clamped_drawing_area;
  gpu_backend_counters_t      counters;
};

extern gpu_backend_t* g_gpu_backend;

/* Lifecycle. */
gpu_backend_t* gpu_backend_create_software(void);
void           gpu_backend_destroy(gpu_backend_t* be);

/* Reset accumulated counters. */
void gpu_backend_reset_statistics(gpu_backend_t* be);

/* Synchronous dispatch wrappers. */
void gpu_backend_read_vram (const gpu_backend_read_vram_cmd_t* c);
void gpu_backend_fill_vram (const gpu_backend_fill_vram_cmd_t* c);
void gpu_backend_update_vram(const gpu_backend_update_vram_cmd_t* c, const u16* data);
void gpu_backend_copy_vram (const gpu_backend_copy_vram_cmd_t* c);
void gpu_backend_set_drawing_area(const gpu_backend_set_drawing_area_cmd_t* c);
void gpu_backend_update_clut     (const gpu_backend_update_clut_cmd_t* c);
void gpu_backend_clear_cache(void);
void gpu_backend_clear_vram (void);
void gpu_backend_update_display(const gpu_backend_update_display_cmd_t* c);
void gpu_backend_flush_render(void);

void gpu_backend_draw_polygon  (const gpu_backend_draw_polygon_cmd_t* c,
                                const gpu_backend_polygon_vertex_t* vertices);
void gpu_backend_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* c);
void gpu_backend_draw_line     (const gpu_backend_draw_line_cmd_t* c,
                                const gpu_backend_line_vertex_t* vertices);
void gpu_backend_draw_precise_polygon(const gpu_backend_draw_precise_polygon_cmd_t* c,
                                      const gpu_backend_precise_polygon_vertex_t* vertices);
void gpu_backend_draw_precise_line   (const gpu_backend_draw_precise_line_cmd_t* c,
                                      const gpu_backend_precise_line_vertex_t* vertices);

#endif /* CUPID_CORE_GPU_BACKEND_H */
