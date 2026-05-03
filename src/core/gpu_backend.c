/*
 * The C++ GPUBackend lives across two threads (CPU + video).  In cupid-ps1 the
 * software back-end runs synchronously on the CPU thread, so the elaborate
 * command-queue / arena allocator goes away; we just dispatch through the
 * vtable.  This file holds the global pointer + the dispatch wrappers; the
 * concrete back-end factory lives in gpu_sw.c.
 */

#include "core/gpu_backend.h"
#include "core/fmv_dump.h"
#include "common/assert.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUBackend);

gpu_backend_t* g_gpu_backend = NULL;

static int gpu_backend_trace_disp_enabled(void)
{
  static int cached = -1;
  if (cached < 0) cached = (getenv("CUPID_TRACE_DISP") != NULL) ? 1 : 0;
  return cached;
}

static u64 s_be_count_poly = 0;
static u64 s_be_count_rect = 0;
static u64 s_be_count_line = 0;
static u64 s_be_count_fill = 0;
static u64 s_be_count_update_vram = 0;
static u64 s_be_count_copy_vram = 0;
static u64 s_be_count_update_clut = 0;
static u64 s_be_count_update_display = 0;

void gpu_backend_destroy(gpu_backend_t* be)
{
  if (!be) return;
  if (be->vtable && be->vtable->destroy)
    be->vtable->destroy(be);
  if (g_gpu_backend == be)
    g_gpu_backend = NULL;
}

void gpu_backend_reset_statistics(gpu_backend_t* be)
{
  if (!be) return;
  memset(&be->counters, 0, sizeof(be->counters));
}

void gpu_backend_read_vram(const gpu_backend_read_vram_cmd_t* c)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->counters.num_reads++;
  g_gpu_backend->vtable->read_vram(g_gpu_backend, c);
}

void gpu_backend_fill_vram(const gpu_backend_fill_vram_cmd_t* c)
{
  if (!g_gpu_backend) return;
  s_be_count_fill++;
  g_gpu_backend->counters.num_writes++;
  g_gpu_backend->vtable->fill_vram(g_gpu_backend, c);
}

void gpu_backend_update_vram(const gpu_backend_update_vram_cmd_t* c, const u16* data)
{
  if (!g_gpu_backend) return;
  s_be_count_update_vram++;
  g_gpu_backend->counters.num_writes++;
  g_gpu_backend->vtable->update_vram(g_gpu_backend, c, data);
}

void gpu_backend_copy_vram(const gpu_backend_copy_vram_cmd_t* c)
{
  if (!g_gpu_backend) return;
  s_be_count_copy_vram++;
  g_gpu_backend->counters.num_copies++;
  g_gpu_backend->vtable->copy_vram(g_gpu_backend, c);
}

void gpu_backend_set_drawing_area(const gpu_backend_set_drawing_area_cmd_t* c)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->vtable->drawing_area_changed(g_gpu_backend, c);
}

void gpu_backend_update_clut(const gpu_backend_update_clut_cmd_t* c)
{
  if (!g_gpu_backend) return;
  s_be_count_update_clut++;
  g_gpu_backend->vtable->update_clut(g_gpu_backend, c);
}

void gpu_backend_clear_cache(void)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->vtable->clear_cache(g_gpu_backend);
}

void gpu_backend_clear_vram(void)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->vtable->clear_vram(g_gpu_backend);
}

void gpu_backend_update_display(const gpu_backend_update_display_cmd_t* c)
{
  if (!g_gpu_backend) return;
  FMV_DUMP_INC_FRAME();
  s_be_count_update_display++;
  if (gpu_backend_trace_disp_enabled() && (s_be_count_update_display % 30u) == 0u) {
    fprintf(stderr, "[disp counters @upd=%llu] poly=%llu rect=%llu line=%llu "
                    "fill=%llu vramwr=%llu vramcpy=%llu clut=%llu\n",
            (unsigned long long)s_be_count_update_display,
            (unsigned long long)s_be_count_poly,
            (unsigned long long)s_be_count_rect,
            (unsigned long long)s_be_count_line,
            (unsigned long long)s_be_count_fill,
            (unsigned long long)s_be_count_update_vram,
            (unsigned long long)s_be_count_copy_vram,
            (unsigned long long)s_be_count_update_clut);
  }
  g_gpu_backend->vtable->update_display(g_gpu_backend, c);
}

void gpu_backend_flush_render(void)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->vtable->flush_render(g_gpu_backend);
}

void gpu_backend_draw_polygon(const gpu_backend_draw_polygon_cmd_t* c,
                              const gpu_backend_polygon_vertex_t* vertices)
{
  if (!g_gpu_backend) return;
  s_be_count_poly++;
  g_gpu_backend->counters.num_primitives++;
  g_gpu_backend->counters.num_vertices += c->base.num_vertices;
  g_gpu_backend->vtable->draw_polygon(g_gpu_backend, c, vertices);
}

void gpu_backend_draw_rectangle(const gpu_backend_draw_rectangle_cmd_t* c)
{
  if (!g_gpu_backend) return;
  s_be_count_rect++;
  g_gpu_backend->counters.num_primitives++;
  g_gpu_backend->counters.num_vertices += 4; /* sprite expands to a quad */
  g_gpu_backend->vtable->draw_rectangle(g_gpu_backend, c);
}

void gpu_backend_draw_line(const gpu_backend_draw_line_cmd_t* c,
                           const gpu_backend_line_vertex_t* vertices)
{
  if (!g_gpu_backend) return;
  s_be_count_line++;
  g_gpu_backend->counters.num_primitives++;
  g_gpu_backend->counters.num_vertices += c->base.num_vertices;
  g_gpu_backend->vtable->draw_line(g_gpu_backend, c, vertices);
}

void gpu_backend_draw_precise_polygon(const gpu_backend_draw_precise_polygon_cmd_t* c,
                                      const gpu_backend_precise_polygon_vertex_t* vertices)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->counters.num_primitives++;
  g_gpu_backend->counters.num_vertices += c->base.num_vertices;
  if (g_gpu_backend->vtable->draw_precise_polygon) {
    g_gpu_backend->vtable->draw_precise_polygon(g_gpu_backend, c, vertices);
  }
}

void gpu_backend_draw_precise_line(const gpu_backend_draw_precise_line_cmd_t* c,
                                   const gpu_backend_precise_line_vertex_t* vertices)
{
  if (!g_gpu_backend) return;
  g_gpu_backend->counters.num_primitives++;
  g_gpu_backend->counters.num_vertices += c->base.num_vertices;
  if (g_gpu_backend->vtable->draw_precise_line) {
    g_gpu_backend->vtable->draw_precise_line(g_gpu_backend, c, vertices);
  }
}
