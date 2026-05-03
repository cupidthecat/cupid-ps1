/*
 * PGXP (Parallel/Precision Geometry Transformation Pipeline) - tracks
 * pseudo-floating-point coordinates through CPU GPRs + GTE registers +
 * memory + GPU vertex submissions to recover the subpixel/subtexel
 * precision lost by the PS1 fixed-point pipeline.
 *
 * - Initialize/Reset/Shutdown lifecycle
 * - Per-MIPS-op CPU_* hooks called by the interpreter and recompiler
 * - GTE_* hooks for the geometry coprocessor
 * - GetPreciseVertex queried by the GPU when submitting vertices
 *
 * Per-shadow state lives in cpu_state_t (g_cpu_state.pgxp_gpr/_cop0/_gte)
 * so save-state passes cover it for free.  Memory shadow + vertex cache
 * are file-static (lazy-allocated in pgxp_initialize).
 */

#include "cpu_pgxp.h"

#include "bus.h"
#include "cpu_core.h"
#include "gpu_helpers.h"
#include "gte.h"
#include "settings.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/types.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(PGXP);

enum {
  PGXP_VERTEX_CACHE_WIDTH  = 2048u,
  PGXP_VERTEX_CACHE_HEIGHT = 2048u,
  PGXP_VERTEX_CACHE_SIZE   = PGXP_VERTEX_CACHE_WIDTH * PGXP_VERTEX_CACHE_HEIGHT,
  PGXP_SCRATCH_VALUE_COUNT = (CPU_SCRATCHPAD_SIZE / 4u),
  PGXP_MEM_RAM_OFFSET      = PGXP_SCRATCH_VALUE_COUNT,
};

enum {
  PGXP_GTE_SXY0 = 12u,
  PGXP_GTE_SXY1 = 13u,
  PGXP_GTE_SXY2 = 14u,
  PGXP_GTE_SXYP = 15u,
};

#define LOWORD_U16(v)        ((u16)(v))
#define HIWORD_U16(v)        ((u16)((u32)(v) >> 16))
#define LOWORD_S16(v)        ((s16)(u16)(v))
#define HIWORD_S16(v)        ((s16)(u16)((u32)(v) >> 16))
#define SET_LOWORD(v, lo)    (((u32)(v) & 0xFFFF0000u) | (u32)(u16)(lo))
#define SET_HIWORD(v, hi)    (((u32)(v) & 0x0000FFFFu) | ((u32)(hi) << 16))

static const pgxp_value_t INVALID_VALUE = { 0 };

static pgxp_value_t* s_mem          = NULL;
static u32           s_mem_count    = 0u;
static pgxp_value_t* s_vertex_cache = NULL;
static u32           s_invalid_writes = 0u;

static inline double f16Sign(double v)
{
  const s32 s = (s32)(s64)(v * (USHRT_MAX + 1.0));
  return (double)s / (double)(USHRT_MAX + 1);
}
static inline double f16Unsign(double v)   { return v >= 0.0 ? v : (v + (USHRT_MAX + 1.0)); }
static inline double f16Overflow(double v) { return (double)((s64)v >> 16); }

static inline pgxp_value_t* get_rt_value(cpu_instruction_t instr)
{
  return &g_cpu_state.pgxp_gpr[(u8)instr.r.rt];
}

static inline pgxp_value_t* get_rd_value(cpu_instruction_t instr)
{
  return &g_cpu_state.pgxp_gpr[(u8)instr.r.rd];
}

static inline pgxp_value_t* validate_and_get_rs(cpu_instruction_t instr, u32 rs_val)
{
  pgxp_value_t* p = &g_cpu_state.pgxp_gpr[(u8)instr.r.rs];
  pgxp_value_validate(p, rs_val);
  return p;
}

static inline pgxp_value_t* validate_and_get_rt(cpu_instruction_t instr, u32 rt_val)
{
  pgxp_value_t* p = &g_cpu_state.pgxp_gpr[(u8)instr.r.rt];
  pgxp_value_validate(p, rt_val);
  return p;
}

static inline void set_rt_value(cpu_instruction_t instr, const pgxp_value_t* val)
{
  g_cpu_state.pgxp_gpr[(u8)instr.r.rt] = *val;
}

static inline void set_rt_value_with(cpu_instruction_t instr, const pgxp_value_t* val, u32 rt_val)
{
  pgxp_value_t* p = &g_cpu_state.pgxp_gpr[(u8)instr.r.rt];
  *p = *val;
  p->value = rt_val;
}

static inline void push_screen_xy_fifo(void)
{
  g_cpu_state.pgxp_gte[PGXP_GTE_SXY0] = g_cpu_state.pgxp_gte[PGXP_GTE_SXY1];
  g_cpu_state.pgxp_gte[PGXP_GTE_SXY1] = g_cpu_state.pgxp_gte[PGXP_GTE_SXY2];
  g_cpu_state.pgxp_gte[PGXP_GTE_SXY2] = g_cpu_state.pgxp_gte[PGXP_GTE_SXYP];
}

/* GetPtr: address → memory shadow slot, or NULL if out of range. */
static pgxp_value_t* pgxp_get_ptr(u32 addr)
{
  if ((addr & CPU_SCRATCHPAD_ADDR_MASK) == CPU_SCRATCHPAD_ADDR)
    return &s_mem[((addr & CPU_SCRATCHPAD_OFFSET_MASK) >> 2)];

  const u32 paddr = addr & 0x1FFFFFFFu;
  if (paddr < BUS_RAM_MIRROR_END)
    return &s_mem[PGXP_MEM_RAM_OFFSET + ((paddr & bus_get_ram_mask()) >> 2)];

  return NULL;
}

static const pgxp_value_t* validate_and_load_mem(u32 addr, u32 value)
{
  pgxp_value_t* p = pgxp_get_ptr(addr);
  if (!p) return &INVALID_VALUE;
  pgxp_value_validate(p, value);
  return p;
}

static void validate_and_load_mem16(pgxp_value_t* dest, u32 addr, u32 value, bool sign)
{
  pgxp_value_t* p = pgxp_get_ptr(addr);
  if (!p) { *dest = INVALID_VALUE; return; }

  const bool hiword = ((addr & 2u) != 0u);

  if (hiword)
    p->flags = (Truncate16(p->value >> 16) == Truncate16(value)) ? p->flags : (p->flags & ~PGXP_VALID_Y);
  else
    p->flags = (Truncate16(p->value)       == Truncate16(value)) ? p->flags : (p->flags & ~PGXP_VALID_X);

  *dest = *p;

  if (hiword) {
    dest->x     = dest->y;
    dest->flags = (dest->flags & ~PGXP_VALID_X) | ((dest->flags & PGXP_VALID_Y) >> 1);
  }

  if (dest->flags & PGXP_VALID_X) {
    dest->y = (dest->x < 0.0f) ? (sign ? -1.0f : 0.0f) : 0.0f;
    dest->flags |= PGXP_VALID_Y;
  } else {
    dest->y = 0.0f;
    dest->flags &= ~PGXP_VALID_Y;
  }
  dest->value = value;
}

static void write_mem(u32 addr, const pgxp_value_t* value)
{
  pgxp_value_t* p = pgxp_get_ptr(addr);
  if (!p) { s_invalid_writes++; return; }
  *p = *value;
  p->flags = (value->flags & ~(PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ)) |
             ((value->flags & PGXP_VALID_Z) ? (PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ) : 0u);
}

static void write_mem16(u32 addr, const pgxp_value_t* value)
{
  pgxp_value_t* dest = pgxp_get_ptr(addr);
  if (!dest) { s_invalid_writes++; return; }

  const bool hiword = ((addr & 2u) != 0u);
  if (hiword) {
    dest->y     = value->x;
    dest->flags = (dest->flags & ~PGXP_VALID_Y) | ((value->flags & PGXP_VALID_X) << 1);
    dest->value = (dest->value & 0x0000FFFFu) | (value->value << 16);
  } else {
    dest->x     = value->x;
    dest->flags = (dest->flags & ~PGXP_VALID_X) | (value->flags & PGXP_VALID_X);
    dest->value = (dest->value & 0xFFFF0000u) | (value->value & 0x0000FFFFu);
  }

  if (value->flags & PGXP_VALID_Z) {
    dest->z      = value->z;
    dest->flags |= PGXP_VALID_Z | (hiword ? PGXP_VALID_HIGHZ : PGXP_VALID_LOWZ);
  } else {
    dest->flags &= hiword ? ~(u32)PGXP_VALID_HIGHZ : ~(u32)PGXP_VALID_LOWZ;
    if ((dest->flags & PGXP_VALID_Z) && !(dest->flags & (PGXP_VALID_HIGHZ | PGXP_VALID_LOWZ)))
      dest->flags &= ~(u32)PGXP_VALID_Z;
  }
}

static inline void copy_z_if_missing(pgxp_value_t* dst, const pgxp_value_t* src)
{
  dst->z      = (dst->flags & PGXP_VALID_Z) ? dst->z : src->z;
  dst->flags |= (src->flags & PGXP_VALID_Z);
}

static inline void select_z(float* dst_z, u32* dst_flags,
                            const pgxp_value_t* a, const pgxp_value_t* b)
{
  /* Prefer b if a missing Z, or a is tainted while b is precise. */
  *dst_z = (!(a->flags & PGXP_VALID_Z) ||
            ((a->flags & PGXP_VALID_TAINTED_Z) &&
             ((b->flags & (PGXP_VALID_Z | PGXP_VALID_TAINTED_Z)) == PGXP_VALID_Z))) 
             ? b->z : a->z;
  *dst_flags |= ((a->flags | b->flags) & PGXP_VALID_Z);
}

static size_t pgxp_memory_value_count(void)
{
  return PGXP_SCRATCH_VALUE_COUNT + (bus_get_ram_size() / 4u);
}

void pgxp_initialize(void)
{
  memset(g_cpu_state.pgxp_gpr,  0, sizeof(g_cpu_state.pgxp_gpr));
  memset(g_cpu_state.pgxp_cop0, 0, sizeof(g_cpu_state.pgxp_cop0));
  memset(g_cpu_state.pgxp_gte,  0, sizeof(g_cpu_state.pgxp_gte));

  if (!s_mem) {
    s_mem_count = (u32)(PGXP_SCRATCH_VALUE_COUNT + (BUS_RAM_8MB_SIZE >> 2));
    s_mem = (pgxp_value_t*)calloc(s_mem_count, sizeof(pgxp_value_t));
    if (!s_mem) {
      ERROR_LOG("PGXP: failed to allocate memory shadow (%u entries)", s_mem_count);
      s_mem_count = 0u;
    }
  }

  if (g_settings.gpu_pgxp_vertex_cache && !s_vertex_cache) {
    s_vertex_cache = (pgxp_value_t*)calloc(PGXP_VERTEX_CACHE_SIZE, sizeof(pgxp_value_t));
    if (!s_vertex_cache) {
      ERROR_LOG("PGXP: failed to allocate vertex cache, disabling.");
      g_settings.gpu_pgxp_vertex_cache = false;
    }
  }

  if (s_vertex_cache)
    memset(s_vertex_cache, 0, sizeof(pgxp_value_t) * PGXP_VERTEX_CACHE_SIZE);

  s_invalid_writes = 0u;
}

void pgxp_reset(void)
{
  memset(g_cpu_state.pgxp_gpr,  0, sizeof(g_cpu_state.pgxp_gpr));
  memset(g_cpu_state.pgxp_cop0, 0, sizeof(g_cpu_state.pgxp_cop0));
  memset(g_cpu_state.pgxp_gte,  0, sizeof(g_cpu_state.pgxp_gte));
  if (s_mem)
    memset(s_mem, 0, sizeof(pgxp_value_t) * s_mem_count);
  if (g_settings.gpu_pgxp_vertex_cache && s_vertex_cache)
    memset(s_vertex_cache, 0, sizeof(pgxp_value_t) * PGXP_VERTEX_CACHE_SIZE);
  s_invalid_writes = 0u;
}

void pgxp_shutdown(void)
{
  free(s_vertex_cache); s_vertex_cache = NULL;
  free(s_mem);          s_mem = NULL; s_mem_count = 0u;
  memset(g_cpu_state.pgxp_gpr,  0, sizeof(g_cpu_state.pgxp_gpr));
  memset(g_cpu_state.pgxp_cop0, 0, sizeof(g_cpu_state.pgxp_cop0));
  memset(g_cpu_state.pgxp_gte,  0, sizeof(g_cpu_state.pgxp_gte));
}

bool pgxp_should_save_state(void)
{
  return g_settings.gpu_pgxp_enable && g_settings.gpu_pgxp_cpu;
}

size_t pgxp_get_state_size(bool enable_8mb_memory)
{
  const size_t value_count = PGXP_SCRATCH_VALUE_COUNT +
                             ((enable_8mb_memory ? BUS_RAM_8MB_SIZE : BUS_RAM_2MB_SIZE) >> 2);
  const size_t base_size = sizeof(g_cpu_state.pgxp_gpr) +
                           sizeof(g_cpu_state.pgxp_cop0) +
                           sizeof(g_cpu_state.pgxp_gte) +
                           sizeof(pgxp_value_t) * value_count;
  const size_t vertex_cache_size = sizeof(pgxp_value_t) * PGXP_VERTEX_CACHE_SIZE;
  return base_size + (g_settings.gpu_pgxp_vertex_cache ? vertex_cache_size : 0u);
}

void pgxp_do_state(state_wrapper_t* sw)
{
  /* Gate body on a 1-byte present flag for backwards-compat with old
   * states that omitted PGXP shadows. */
  bool present = pgxp_should_save_state();
  state_wrapper_do_bool(sw, &present);
  if (!present) return;

  state_wrapper_do_bytes(sw, g_cpu_state.pgxp_gpr,  sizeof(g_cpu_state.pgxp_gpr));
  state_wrapper_do_bytes(sw, g_cpu_state.pgxp_cop0, sizeof(g_cpu_state.pgxp_cop0));
  state_wrapper_do_bytes(sw, g_cpu_state.pgxp_gte,  sizeof(g_cpu_state.pgxp_gte));

  if (s_mem)
    state_wrapper_do_bytes(sw, s_mem, sizeof(pgxp_value_t) * pgxp_memory_value_count());

  if (s_vertex_cache)
    state_wrapper_do_bytes(sw, s_vertex_cache, sizeof(pgxp_value_t) * PGXP_VERTEX_CACHE_SIZE);
}

static void cache_vertex(u32 value, const pgxp_value_t* vertex)
{
  if (!s_vertex_cache) return;
  const s16 sx = (s16)(value & 0xFFFFu);
  const s16 sy = (s16)(value >> 16);
  if (sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1023)
    s_vertex_cache[(sy + 1024) * PGXP_VERTEX_CACHE_WIDTH + (sx + 1024)] = *vertex;
}

static pgxp_value_t* get_cached_vertex(u32 value)
{
  if (!s_vertex_cache) return NULL;
  const s16 sx = (s16)(value & 0xFFFFu);
  const s16 sy = (s16)(value >> 16);
  return (sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1013)
           ? &s_vertex_cache[(sy + 1024) * PGXP_VERTEX_CACHE_WIDTH + (sx + 1024)]
           : NULL;
}

static float truncate_vertex_position(float p)
{
  const s32 int_part = (s32)p;
  const float int_f  = (float)int_part;
  return (float)truncate_gpu_vertex_position(int_part) + (p - int_f);
}

static bool is_within_tolerance(float px, float py, int ix, int iy)
{
  const float tol = g_settings.gpu_pgxp_tolerance;
  if (tol < 0.0f) return true;
  return (fabsf(px - (float)ix) <= tol && fabsf(py - (float)iy) <= tol);
}

bool pgxp_get_precise_vertex(u32 addr, u32 value, int x, int y,
                             int x_offs, int y_offs,
                             float* out_x, float* out_y, float* out_w)
{
  const pgxp_value_t* vert = pgxp_get_ptr(addr);
  if (vert && (vert->flags & PGXP_VALID_XY) == PGXP_VALID_XY && vert->value == value) {
    *out_x = truncate_vertex_position(vert->x) + (float)x_offs;
    *out_y = truncate_vertex_position(vert->y) + (float)y_offs;
    *out_w = vert->z / (float)GTE_MAX_Z;
    if (is_within_tolerance(*out_x, *out_y, x, y))
      return ((vert->flags & PGXP_VALID_Z) == PGXP_VALID_Z);
  }

  if (g_settings.gpu_pgxp_vertex_cache) {
    vert = get_cached_vertex(value);
    if (vert && (vert->flags & PGXP_VALID_XY) == PGXP_VALID_XY) {
      *out_x = truncate_vertex_position(vert->x) + (float)x_offs;
      *out_y = truncate_vertex_position(vert->y) + (float)y_offs;
      *out_w = vert->z / (float)GTE_MAX_Z;
      if (is_within_tolerance(*out_x, *out_y, x, y))
        return false;
    }
  }

  *out_x = (float)x;
  *out_y = (float)y;
  *out_w = 1.0f;
  return false;
}

void pgxp_gte_rtps(float x, float y, float z, u32 value)
{
  pgxp_value_t* sxyp = &g_cpu_state.pgxp_gte[PGXP_GTE_SXYP];
  sxyp->x = x; sxyp->y = y; sxyp->z = z;
  sxyp->value = value;
  sxyp->flags = PGXP_VALID_ALL;
  push_screen_xy_fifo();
  if (g_settings.gpu_pgxp_vertex_cache)
    cache_vertex(value, sxyp);
}

bool pgxp_gte_has_precise_vertices(u32 sxy0, u32 sxy1, u32 sxy2)
{
  pgxp_value_t* s0 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY0];
  pgxp_value_t* s1 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY1];
  pgxp_value_t* s2 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY2];
  pgxp_value_validate(s0, sxy0);
  pgxp_value_validate(s1, sxy1);
  pgxp_value_validate(s2, sxy2);
  return ((s0->flags & s1->flags & s2->flags & PGXP_VALID_XYZ) == PGXP_VALID_XYZ);
}

float pgxp_gte_nclip(void)
{
  const pgxp_value_t* s0 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY0];
  const pgxp_value_t* s1 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY1];
  const pgxp_value_t* s2 = &g_cpu_state.pgxp_gte[PGXP_GTE_SXY2];
  float n = (s0->x * s1->y) + (s1->x * s2->y) + (s2->x * s0->y)
          - (s0->x * s2->y) - (s1->x * s0->y) - (s2->x * s1->y);
  const float a = fabsf(n);
  if (0.1f < a && a < 1.0f) n += (n < 0.0f) ? -1.0f : 1.0f;
  return n;
}

static void cpu_mtc2_inner(u32 reg, const pgxp_value_t* value, u32 val)
{
  switch (reg) {
    case 15: /* push FIFO */
      g_cpu_state.pgxp_gte[PGXP_GTE_SXYP] = *value;
      push_screen_xy_fifo();
      return;
    case 29: /* read-only */
    case 31:
      return;
    default: {
      pgxp_value_t* g = &g_cpu_state.pgxp_gte[reg];
      *g = *value;
      g->value = val;
      return;
    }
  }
}

void pgxp_cpu_mfc2(cpu_instruction_t instr, u32 rd_val)
{
  const u32 idx = cpu_cop2_index(instr);
  pgxp_value_t* p = &g_cpu_state.pgxp_gte[idx];
  pgxp_value_validate(p, rd_val);
  set_rt_value_with(instr, p, rd_val);
}

void pgxp_cpu_mtc2(cpu_instruction_t instr, u32 rt_val)
{
  const u32 idx = cpu_cop2_index(instr);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  cpu_mtc2_inner(idx, prt, rt_val);
}

void pgxp_cpu_lwc2(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  const pgxp_value_t* mem = validate_and_load_mem(addr, rt_val);
  cpu_mtc2_inner((u32)instr.r.rt, mem, rt_val);
}

void pgxp_cpu_swc2(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  const u32 idx = (u32)instr.r.rt;
  pgxp_value_t* p = &g_cpu_state.pgxp_gte[idx];
  pgxp_value_validate(p, rt_val);
  write_mem(addr, p);
}

void pgxp_cpu_lw(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  set_rt_value(instr, validate_and_load_mem(addr, rt_val));
}

void pgxp_cpu_lbx(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  (void)addr; (void)rt_val;
  set_rt_value(instr, &INVALID_VALUE);
}

void pgxp_cpu_lh(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  validate_and_load_mem16(get_rt_value(instr), addr, rt_val, true);
}

void pgxp_cpu_lhu(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  validate_and_load_mem16(get_rt_value(instr), addr, rt_val, false);
}

void pgxp_cpu_sb(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  (void)instr; (void)rt_val;
  write_mem(addr, &INVALID_VALUE);
}

void pgxp_cpu_sh(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  write_mem16(addr, prt);
}

void pgxp_cpu_sw(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  write_mem(addr, prt);
}

void pgxp_cpu_lwx(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  const u32 aligned = addr & ~3u;
  pgxp_value_t* pmem = pgxp_get_ptr(aligned);
  if (!pmem) return;
  u32 mem_val = 0u;
  if (!cpu_safe_read_memory_word(aligned, &mem_val)) return;
  pgxp_value_validate(pmem, mem_val);

  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  const u32 byte_shift = addr & 3u;
  const bool is_lwl = (instr.i.op == 0x22u);

  if (is_lwl) {
    const u32 bit_shift = byte_shift * 8u;
    const u32 mixed = (rt_val & (0x00FFFFFFu >> bit_shift)) | (mem_val << (24u - bit_shift));
    switch (byte_shift) {
      case 0:
        prt->y = (float)(s16)(mixed >> 16);
        prt->value = mixed;
        prt->flags &= ~(u32)PGXP_VALID_Y;
        break;
      case 1:
        prt->y = pmem->x;
        prt->z = (pmem->flags & PGXP_VALID_LOWZ) ? pmem->z : prt->z;
        prt->value = mixed;
         prt->flags = (prt->flags & ~(u32)PGXP_VALID_Y) |
                     ((pmem->flags & PGXP_VALID_X) << 1) | 
                     ((pmem->flags & PGXP_VALID_LOWZ) ? PGXP_VALID_Z : 0u);
        break;
      case 2:
        prt->x = (float)(s16)mixed;
        prt->y = (float)(s16)(mixed >> 16);
        prt->value = mixed;
        prt->flags &= ~(u32)(PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_Z);
        break;
      case 3:
        *prt = *pmem;
        prt->value = mixed;
        break;
      default: break;
    }
  } else {
    const u32 bit_shift = byte_shift * 8u;
    const u32 mixed = (rt_val & (0xFFFFFF00u << (24u - bit_shift))) | (mem_val >> bit_shift);
    switch (byte_shift) {
      case 0:
        *prt = *pmem;
        prt->value = mixed;
        break;
      case 1:
        prt->x = (float)(s16)mixed;
        prt->y = (float)(s16)(mixed >> 16);
        prt->value = mixed;
        prt->flags &= ~(u32)(PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_Z);
        break;
      case 2:
        prt->x = pmem->y;
        prt->z = (pmem->flags & PGXP_VALID_HIGHZ) ? pmem->z : prt->z;
        prt->value = mixed;
         prt->flags = (prt->flags & ~(u32)PGXP_VALID_X) |
                     ((pmem->flags & PGXP_VALID_Y) >> 1) | 
                     ((pmem->flags & PGXP_VALID_HIGHZ) ? PGXP_VALID_Z : 0u);
        break;
      case 3:
        prt->x = (float)(s16)mixed;
        prt->value = mixed;
        prt->flags &= ~(u32)PGXP_VALID_X;
        break;
      default: break;
    }
  }
}

void pgxp_cpu_swx(cpu_instruction_t instr, u32 addr, u32 rt_val)
{
  const u32 aligned = addr & ~3u;
  pgxp_value_t* pmem = pgxp_get_ptr(aligned);
  if (!pmem) return;
  u32 mem_val = 0u;
  if (!cpu_safe_read_memory_word(aligned, &mem_val)) return;
  pgxp_value_validate(pmem, mem_val);

  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  const u32 byte_shift = addr & 3u;
  const bool is_swl = (instr.i.op == 0x2Au);

  if (is_swl) {
    const u32 bit_shift = byte_shift * 8u;
    const u32 mixed = (mem_val & (0xFFFFFF00u << bit_shift)) | (rt_val >> (24u - bit_shift));
    switch (byte_shift) {
      case 0:
        pmem->x = (float)(s16)mixed;
        pmem->value = mixed;
        pmem->flags = (pmem->flags & ~(u32)(PGXP_VALID_X | PGXP_VALID_Z | PGXP_VALID_LOWZ)) |
                      ((pmem->flags & PGXP_VALID_HIGHZ) ? PGXP_VALID_Z : 0u);
        break;
      case 1:
        pmem->x = prt->y;
        pmem->z = (prt->flags & PGXP_VALID_Z) ? prt->z : pmem->z;
        pmem->value = mixed;
         pmem->flags = (pmem->flags & ~(u32)(PGXP_VALID_X | PGXP_VALID_Z | PGXP_VALID_LOWZ)) |
                      ((prt->flags & PGXP_VALID_Y) >> 1) |
                      ((prt->flags & PGXP_VALID_Z) ? (PGXP_VALID_Z | PGXP_VALID_LOWZ) : 0u) | 
                      ((pmem->flags & PGXP_VALID_HIGHZ) ? PGXP_VALID_Z : 0u);
        break;
      case 2:
        pmem->x = (float)(s16)mixed;
        pmem->y = (float)(s16)(mixed >> 16);
        pmem->value = mixed;
        pmem->flags &= ~(u32)(PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_Z |
                              PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ);
        break;
      case 3:
        *pmem = *prt;
        pmem->value = mixed;
        pmem->flags = (prt->flags & ~(u32)(PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ)) |
                      ((prt->flags & PGXP_VALID_Z) ? (PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ) : 0u);
        break;
      default: break;
    }
  } else {
    const u32 bit_shift = byte_shift * 8u;
    const u32 mixed = (mem_val & (0x00FFFFFFu >> (24u - bit_shift))) | (rt_val << bit_shift);
    switch (byte_shift) {
      case 0:
        *pmem = *prt;
        pmem->value = mixed;
        pmem->flags = (prt->flags & ~(u32)(PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ)) |
                      ((prt->flags & PGXP_VALID_Z) ? (PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ) : 0u);
        break;
      case 1:
        pmem->x = (float)(s16)mixed;
        pmem->y = (float)(s16)(mixed >> 16);
        pmem->value = mixed;
        pmem->flags &= ~(u32)(PGXP_VALID_X | PGXP_VALID_Y |
                              PGXP_VALID_LOWZ | PGXP_VALID_HIGHZ);
        break;
      case 2:
        pmem->y = prt->x;
        pmem->z = (prt->flags & PGXP_VALID_Z) ? prt->z : pmem->z;
        pmem->value = mixed;
         pmem->flags = (pmem->flags & ~(u32)(PGXP_VALID_X | PGXP_VALID_Z | PGXP_VALID_HIGHZ)) |
                      ((prt->flags & PGXP_VALID_X) << 1) |
                      ((prt->flags & PGXP_VALID_Z) ? (PGXP_VALID_Z | PGXP_VALID_HIGHZ) : 0u) | 
                      ((pmem->flags & PGXP_VALID_LOWZ) ? PGXP_VALID_Z : 0u);
        break;
      case 3:
        pmem->y = (float)(s16)mixed;
        pmem->value = mixed;
        pmem->flags = (pmem->flags & ~(u32)(PGXP_VALID_X | PGXP_VALID_Z | PGXP_VALID_HIGHZ)) |
                      ((pmem->flags & PGXP_VALID_LOWZ) ? PGXP_VALID_Z : 0u);
        break;
      default: break;
    }
  }
}

void pgxp_cpu_move(u32 rd, u32 rs, u32 rs_val)
{
  if (rd >= PGXP_GPR_COUNT || rs >= PGXP_GPR_COUNT) return;
  pgxp_value_t* prs = &g_cpu_state.pgxp_gpr[rs];
  pgxp_value_validate(prs, rs_val);
  g_cpu_state.pgxp_gpr[rd] = *prs;
}

void pgxp_cpu_move_packed(u32 rd_and_rs, u32 rs_val)
{
  pgxp_cpu_move((rd_and_rs >> 8) & 0xFFu, rd_and_rs & 0xFFu, rs_val);
}

void pgxp_cpu_lui(cpu_instruction_t instr)
{
  pgxp_value_t* prt = get_rt_value(instr);
  prt->x = 0.0f;
  prt->y = (float)cpu_instr_imm_s16(instr);
  prt->z = 0.0f;
  prt->value = cpu_instr_imm_zext32(instr) << 16;
  prt->flags = PGXP_VALID_XY;
}

void pgxp_cpu_addi(cpu_instruction_t instr, u32 rs_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  const u32 imm = cpu_instr_imm_sext32(instr);
  pgxp_value_t* prt = get_rt_value(instr);
  *prt = *prs;
  if (imm == 0u) return;

  if (rs_val == 0u) {
    prt->x = (float)LOWORD_S16(imm);
    prt->y = (float)HIWORD_S16(imm);
    prt->flags |= PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_TAINTED_Z;
    prt->value = imm;
    return;
  }

  prt->x = (float)f16Unsign(prt->x);
  prt->x += (float)LOWORD_U16(imm);
  const float of = (prt->x > USHRT_MAX) ? 1.0f : ((prt->x < 0.0f) ? -1.0f : 0.0f);
  prt->x = (float)f16Sign(prt->x);
  prt->y += HIWORD_S16(imm) + of;
  prt->y += (prt->y > SHRT_MAX) ? -(USHRT_MAX + 1.0f)
          : (prt->y < SHRT_MIN) ?  (USHRT_MAX + 1.0f) : 0.0f;
  prt->value = rs_val + imm;
  prt->flags |= PGXP_VALID_TAINTED_Z;
}

void pgxp_cpu_andi(cpu_instruction_t instr, u32 rs_val)
{
  const u32 imm = cpu_instr_imm_zext32(instr);
  const u32 rt_val = rs_val & imm;
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = get_rt_value(instr);

  prt->y = 0.0f;
  prt->z = prs->z;
  prt->value = rt_val;
  prt->flags = prs->flags | PGXP_VALID_Y | PGXP_VALID_TAINTED_Z;

  if (imm == 0u) {
    prt->x = 0.0f;
    prt->flags |= PGXP_VALID_X;
  } else if (imm == 0xFFFFu) {
    prt->x = prs->x;
  } else {
    prt->x = (float)LOWORD_S16(rt_val);
    prt->flags |= PGXP_VALID_X;
  }
}

void pgxp_cpu_ori(cpu_instruction_t instr, u32 rs_val)
{
  const u32 imm = cpu_instr_imm_zext32(instr);
  const u32 rt_val = rs_val | imm;
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = get_rt_value(instr);
  *prt = *prs;
  prt->value = rt_val;
  if (imm != 0u) {
    prt->x = (float)LOWORD_S16(rt_val);
    prt->flags |= PGXP_VALID_X | PGXP_VALID_TAINTED_Z;
  }
}

void pgxp_cpu_xori(cpu_instruction_t instr, u32 rs_val)
{
  const u32 imm = cpu_instr_imm_zext32(instr);
  const u32 rt_val = rs_val ^ imm;
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = get_rt_value(instr);
  *prt = *prs;
  prt->value = rt_val;
  if (imm != 0u) {
    prt->x = (float)LOWORD_S16(rt_val);
    prt->flags |= PGXP_VALID_X | PGXP_VALID_TAINTED_Z;
  }
}

void pgxp_cpu_slti(cpu_instruction_t instr, u32 rs_val)
{
  const s32 imm = cpu_instr_imm_s16(instr);
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  const float fimmx = (float)imm;
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;
  pgxp_value_t* prt = get_rt_value(instr);
  prt->x = (pgxp_value_get_valid_y(prs, rs_val) < fimmy ||
            pgxp_value_get_valid_x(prs, rs_val) < fimmx) ? 1.0f : 0.0f;
  prt->y = 0.0f;
  prt->z = prs->z;
  prt->flags = prs->flags | PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_TAINTED_Z;
  prt->value = BoolToUInt32((s32)rs_val < imm);
}

void pgxp_cpu_sltiu(cpu_instruction_t instr, u32 rs_val)
{
  const u32 imm = cpu_instr_imm_u16(instr);
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  const float fimmx = (float)(s16)imm;
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;
  pgxp_value_t* prt = get_rt_value(instr);
  prt->x = (f16Unsign(pgxp_value_get_valid_y(prs, rs_val)) < f16Unsign(fimmy) ||
            f16Unsign(pgxp_value_get_valid_x(prs, rs_val)) < fimmx) ? 1.0f : 0.0f;
  prt->y = 0.0f;
  prt->z = prs->z;
  prt->flags = prs->flags | PGXP_VALID_X | PGXP_VALID_Y | PGXP_VALID_TAINTED_Z;
  prt->value = BoolToUInt32(rs_val < imm);
}

void pgxp_cpu_add(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = get_rd_value(instr);

  if (rt_val == 0u) {
    *prd = *prs;
    copy_z_if_missing(prd, prt);
  } else if (rs_val == 0u) {
    *prd = *prt;
    copy_z_if_missing(prd, prs);
  } else {
    const double x = f16Unsign(pgxp_value_get_valid_x(prs, rs_val))
                   + f16Unsign(pgxp_value_get_valid_x(prt, rt_val));
    const float of = (x > USHRT_MAX) ? 1.0f : (x < 0.0) ? -1.0f : 0.0f;
    prd->x = (float)f16Sign(x);
    prd->y = pgxp_value_get_valid_y(prs, rs_val) + pgxp_value_get_valid_y(prt, rt_val) + of;
    prd->y += (prd->y > SHRT_MAX) ? -(USHRT_MAX + 1.0f)
            : (prd->y < SHRT_MIN) ?  (USHRT_MAX + 1.0f) : 0.0f;
    prd->value = rs_val + rt_val;
    prd->flags = prs->flags | (prt->flags & PGXP_VALID_XY) | PGXP_VALID_TAINTED_Z;
    select_z(&prd->z, &prd->flags, prs, prt);
  }
}

void pgxp_cpu_sub(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = get_rd_value(instr);

  if (rt_val == 0u) {
    *prd = *prs;
    copy_z_if_missing(prd, prt);
  } else {
    const double x = f16Unsign(pgxp_value_get_valid_x(prs, rs_val))
                   - f16Unsign(pgxp_value_get_valid_x(prt, rt_val));
    const float of = (x > USHRT_MAX) ? 1.0f : (x < 0.0) ? -1.0f : 0.0f;
    prd->x = (float)f16Sign(x);
    prd->y = pgxp_value_get_valid_y(prs, rs_val) - (pgxp_value_get_valid_y(prt, rt_val) - of);
    prd->y += (prd->y > SHRT_MAX) ? -(USHRT_MAX + 1.0f)
            : (prd->y < SHRT_MIN) ?  (USHRT_MAX + 1.0f) : 0.0f;
    prd->value = rs_val - rt_val;
    prd->flags = prs->flags | (prt->flags & PGXP_VALID_XY) | PGXP_VALID_TAINTED_Z;
    select_z(&prd->z, &prd->flags, prs, prt);
  }
}

static void cpu_bitwise(cpu_instruction_t instr, u32 rd_val, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);

  float x, y;
  if      (LOWORD_U16(rd_val) == 0u)              x = 0.0f;
  else if (LOWORD_U16(rd_val) == LOWORD_U16(rs_val)) x = pgxp_value_get_valid_x(prs, rs_val);
  else if (LOWORD_U16(rd_val) == LOWORD_U16(rt_val)) x = pgxp_value_get_valid_x(prt, rt_val);
  else                                            x = (float)LOWORD_S16(rd_val);

  if      (HIWORD_U16(rd_val) == 0u)              y = 0.0f;
  else if (HIWORD_U16(rd_val) == HIWORD_U16(rs_val)) y = pgxp_value_get_valid_y(prs, rs_val);
  else if (HIWORD_U16(rd_val) == HIWORD_U16(rt_val)) y = pgxp_value_get_valid_y(prt, rt_val);
  else                                            y = (float)HIWORD_S16(rd_val);

  u32 flags = ((prs->flags | prt->flags) & PGXP_VALID_XY)
                ? (PGXP_VALID_XY | PGXP_VALID_TAINTED_Z) : 0u;
  pgxp_value_t* prd = get_rd_value(instr);
  select_z(&prd->z, &flags, prs, prt);
  prd->x = x; prd->y = y;
  prd->flags = flags;
  prd->value = rd_val;
}

void pgxp_cpu_and(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{ cpu_bitwise(instr, rs_val & rt_val, rs_val, rt_val); }
void pgxp_cpu_or(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{ cpu_bitwise(instr, rs_val | rt_val, rs_val, rt_val); }
void pgxp_cpu_xor(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{ cpu_bitwise(instr, rs_val ^ rt_val, rs_val, rt_val); }
void pgxp_cpu_nor(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{ cpu_bitwise(instr, ~(rs_val | rt_val), rs_val, rt_val); }

void pgxp_cpu_slt(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = get_rd_value(instr);
  prd->x = (pgxp_value_get_valid_y(prs, rs_val) < pgxp_value_get_valid_y(prt, rt_val) ||
            f16Unsign(pgxp_value_get_valid_x(prs, rs_val)) < f16Unsign(pgxp_value_get_valid_x(prt, rt_val)))
             ? 1.0f : 0.0f;
  prd->y = 0.0f;
  prd->z = prs->z;
  prd->flags = prs->flags | PGXP_VALID_TAINTED_Z | PGXP_VALID_X | PGXP_VALID_Y;
  prd->value = BoolToUInt32((s32)rs_val < (s32)rt_val);
}

void pgxp_cpu_sltu(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = get_rd_value(instr);
  prd->x = (f16Unsign(pgxp_value_get_valid_y(prs, rs_val)) < f16Unsign(pgxp_value_get_valid_y(prt, rt_val)) ||
            f16Unsign(pgxp_value_get_valid_x(prs, rs_val)) < f16Unsign(pgxp_value_get_valid_x(prt, rt_val)))
             ? 1.0f : 0.0f;
  prd->y = 0.0f;
  prd->z = prs->z;
  prd->flags = prs->flags | PGXP_VALID_TAINTED_Z | PGXP_VALID_X | PGXP_VALID_Y;
  prd->value = BoolToUInt32(rs_val < rt_val);
}

static inline pgxp_value_t* pgxp_lo(void) { return &g_cpu_state.pgxp_gpr[CPU_REG_LO]; }
static inline pgxp_value_t* pgxp_hi(void) { return &g_cpu_state.pgxp_gpr[CPU_REG_HI]; }

void pgxp_cpu_mult(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* lo  = pgxp_lo();
  pgxp_value_t* hi  = pgxp_hi();
  *lo = *prs;
  copy_z_if_missing(lo, prs);
  *hi = *lo;

  const float rsx = pgxp_value_get_valid_x(prs, rs_val);
  const float rsy = pgxp_value_get_valid_y(prs, rs_val);
  const float rtx = pgxp_value_get_valid_x(prt, rt_val);
  const float rty = pgxp_value_get_valid_y(prt, rt_val);

  const double xx = f16Unsign(rsx) * f16Unsign(rtx);
  const double xy = f16Unsign(rsx) * (rty);
  const double yx = rsy * f16Unsign(rtx);
  const double yy = rsy * rty;

  const double lx = xx;
  const double ly = f16Overflow(xx) + (xy + yx);
  const double hx = f16Overflow(ly) + yy;
  const double hy = f16Overflow(hx);

  lo->x = (float)f16Sign(lx);
  lo->y = (float)f16Sign(ly);
  lo->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);
  hi->x = (float)f16Sign(hx);
  hi->y = (float)f16Sign(hy);
  hi->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  const u64 result = (u64)((s64)SignExtend64(rs_val) * (s64)SignExtend64(rt_val));
  hi->value = Truncate32(result >> 32);
  lo->value = Truncate32(result);
}

void pgxp_cpu_multu(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* lo  = pgxp_lo();
  pgxp_value_t* hi  = pgxp_hi();
  *lo = *prs;
  copy_z_if_missing(lo, prs);
  *hi = *lo;

  const float rsx = pgxp_value_get_valid_x(prs, rs_val);
  const float rsy = pgxp_value_get_valid_y(prs, rs_val);
  const float rtx = pgxp_value_get_valid_x(prt, rt_val);
  const float rty = pgxp_value_get_valid_y(prt, rt_val);

  const double xx = f16Unsign(rsx) * f16Unsign(rtx);
  const double xy = f16Unsign(rsx) * f16Unsign(rty);
  const double yx = f16Unsign(rsy) * f16Unsign(rtx);
  const double yy = f16Unsign(rsy) * f16Unsign(rty);

  const double lx = xx;
  const double ly = f16Overflow(xx) + (xy + yx);
  const double hx = f16Overflow(ly) + yy;
  const double hy = f16Overflow(hx);

  lo->x = (float)f16Sign(lx);
  lo->y = (float)f16Sign(ly);
  lo->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);
  hi->x = (float)f16Sign(hx);
  hi->y = (float)f16Sign(hy);
  hi->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  const u64 result = (u64)rs_val * (u64)rt_val;
  hi->value = Truncate32(result >> 32);
  lo->value = Truncate32(result);
}

void pgxp_cpu_div(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* lo  = pgxp_lo();
  pgxp_value_t* hi  = pgxp_hi();
  *lo = *prs;
  copy_z_if_missing(lo, prs);
  *hi = *lo;

  const double vs = f16Unsign(pgxp_value_get_valid_x(prs, rs_val))
                  + pgxp_value_get_valid_y(prs, rs_val) * (double)(1 << 16);
  const double vt = f16Unsign(pgxp_value_get_valid_x(prt, rt_val))
                  + pgxp_value_get_valid_y(prt, rt_val) * (double)(1 << 16);

  const double q = vs / vt;
  lo->y = (float)f16Sign(f16Overflow(q));
  lo->x = (float)f16Sign(q);
  lo->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  const double r = fmod(vs, vt);
  hi->y = (float)f16Sign(f16Overflow(r));
  hi->x = (float)f16Sign(r);
  hi->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  if ((s32)rt_val == 0) {
    lo->value = ((s32)rs_val >= 0) ? 0xFFFFFFFFu : 1u;
    hi->value = (u32)(s32)rs_val;
  } else if (rs_val == 0x80000000u && (s32)rt_val == -1) {
    lo->value = 0x80000000u;
    hi->value = 0u;
  } else {
    lo->value = (u32)((s32)rs_val / (s32)rt_val);
    hi->value = (u32)((s32)rs_val % (s32)rt_val);
  }
}

void pgxp_cpu_divu(cpu_instruction_t instr, u32 rs_val, u32 rt_val)
{
  pgxp_value_t* prs = validate_and_get_rs(instr, rs_val);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* lo  = pgxp_lo();
  pgxp_value_t* hi  = pgxp_hi();
  *lo = *prs;
  copy_z_if_missing(lo, prs);
  *hi = *lo;

  const double vs = f16Unsign(pgxp_value_get_valid_x(prs, rs_val))
                  + f16Unsign(pgxp_value_get_valid_y(prs, rs_val)) * (double)(1 << 16);
  const double vt = f16Unsign(pgxp_value_get_valid_x(prt, rt_val))
                  + f16Unsign(pgxp_value_get_valid_y(prt, rt_val)) * (double)(1 << 16);

  const double q = vs / vt;
  lo->y = (float)f16Sign(f16Overflow(q));
  lo->x = (float)f16Sign(q);
  lo->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  const double r = fmod(vs, vt);
  hi->y = (float)f16Sign(f16Overflow(r));
  hi->x = (float)f16Sign(r);
  hi->flags |= PGXP_VALID_TAINTED_Z | (prt->flags & PGXP_VALID_XY);

  if (rt_val == 0u) {
    lo->value = 0xFFFFFFFFu;
    hi->value = rs_val;
  } else {
    lo->value = rs_val / rt_val;
    hi->value = rs_val % rt_val;
  }
}

static void cpu_sll_inner(cpu_instruction_t instr, u32 rt_val, u32 sh)
{
  const u32 rd_val = rt_val << sh;
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = get_rd_value(instr);
  prd->z = prt->z;
  prd->value = rd_val;

  if (sh >= 32u) {
    prd->x = 0.0f; prd->y = 0.0f;
    prd->flags = prt->flags | PGXP_VALID_XY | PGXP_VALID_TAINTED_Z;
  } else if (sh == 16u) {
    prd->y = prt->x;
    prd->x = 0.0f;
    prd->flags = (prt->flags | PGXP_VALID_TAINTED_Z) | ((prt->flags & PGXP_VALID_Y) >> 1);
  } else if (sh >= 16u) {
    prd->y = (float)f16Sign(f16Unsign((double)prt->x * (double)(1u << (sh - 16u))));
    prd->x = 0.0f;
    prd->flags = (prt->flags | PGXP_VALID_TAINTED_Z) | ((prt->flags & PGXP_VALID_Y) >> 1);
  } else {
    const double x = f16Unsign((double)prt->x) * (double)(1u << sh);
    const double y = f16Unsign((double)prt->y) * (double)(1u << sh) + f16Overflow(x);
    prd->x = (float)f16Sign(x);
    prd->y = (float)f16Sign(y);
    prd->flags = (prt->flags | PGXP_VALID_TAINTED_Z);
  }
}

static void cpu_srx_inner(cpu_instruction_t instr, u32 rt_val, u32 sh,
                          bool sign, bool is_variable)
{
  const u32 rd_val = sign ? (u32)((s32)rt_val >> sh) : (rt_val >> sh);
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);

  double x = prt->x;
  double y = sign ? prt->y : f16Unsign(prt->y);

  const u32 iX = (u32)(s32)LOWORD_S16(rt_val);
  const u32 iY = SET_LOWORD(rt_val, HIWORD_U16(iX));

  const u32 dX = (u32)((s32)iX >> sh);
  const u32 dY = sign ? (u32)((s32)iY >> sh) : (iY >> sh);

  if (LOWORD_S16(dX) != HIWORD_S16(iX))
    x = x / (double)(1u << sh);
  else
    x = (double)LOWORD_S16(dX);

  if (LOWORD_S16(dY) != HIWORD_S16(iX)) {
    if (sh == 16u)
      x = y;
    else if (sh < 16u) {
      x += y * (double)(1u << (16u - sh));
      if (prt->x < 0.0f) x += (double)(1u << (16u - sh));
    } else {
      x += y / (double)(1u << (sh - 16u));
    }
  }

  if ((HIWORD_S16(dY) == 0) || (HIWORD_S16(dY) == -1))
    y = (double)HIWORD_S16(dY);
  else
    y = y / (double)(1u << sh);

  pgxp_value_t* prd = get_rd_value(instr);

  if (sign && !is_variable && !(prt->flags & PGXP_VALID_Z) && sh < 16u) {
    prd->x = (float)LOWORD_S16(rd_val);
    prd->y = (float)HIWORD_S16(rd_val);
    prd->z = 0.0f;
    prd->value = rd_val;
    prd->flags = PGXP_VALID_XY | PGXP_VALID_TAINTED_Z;
  } else {
    prd->x = (float)f16Sign(x);
    prd->y = (float)f16Sign(y);
    prd->z = prt->z;
    prd->value = rd_val;
    prd->flags = prt->flags | PGXP_VALID_TAINTED_Z;
  }
}

void pgxp_cpu_sll(cpu_instruction_t instr, u32 rt_val)
{ cpu_sll_inner(instr, rt_val, instr.r.shamt); }

void pgxp_cpu_sllv(cpu_instruction_t instr, u32 rt_val, u32 rs_val)
{ cpu_sll_inner(instr, rt_val, rs_val & 0x1Fu); }

void pgxp_cpu_srl(cpu_instruction_t instr, u32 rt_val)
{ cpu_srx_inner(instr, rt_val, instr.r.shamt, false, false); }

void pgxp_cpu_srlv(cpu_instruction_t instr, u32 rt_val, u32 rs_val)
{ cpu_srx_inner(instr, rt_val, rs_val & 0x1Fu, false, true); }

void pgxp_cpu_sra(cpu_instruction_t instr, u32 rt_val)
{ cpu_srx_inner(instr, rt_val, instr.r.shamt, true, false); }

void pgxp_cpu_srav(cpu_instruction_t instr, u32 rt_val, u32 rs_val)
{ cpu_srx_inner(instr, rt_val, rs_val & 0x1Fu, true, true); }

void pgxp_cpu_mfc0(cpu_instruction_t instr, u32 rd_val)
{
  const u32 idx = (u32)instr.r.rd;
  if (idx >= PGXP_COP0_COUNT) return;
  pgxp_value_t* prd = &g_cpu_state.pgxp_cop0[idx];
  pgxp_value_validate(prd, rd_val);

  pgxp_value_t* prt = get_rt_value(instr);
  *prt = *prd;
  prt->value = rd_val;
}

void pgxp_cpu_mtc0(cpu_instruction_t instr, u32 rd_val, u32 rt_val)
{
  (void)rd_val;
  const u32 idx = (u32)instr.r.rd;
  if (idx >= PGXP_COP0_COUNT) return;
  pgxp_value_t* prt = validate_and_get_rt(instr, rt_val);
  pgxp_value_t* prd = &g_cpu_state.pgxp_cop0[idx];
  *prd = *prt;
  prt->value = rd_val;
}
