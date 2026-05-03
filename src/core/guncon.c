/*
 * and software-cursor hooks dropped - only the protocol + bindings survive.
 *
 * The light-gun -> framebuffer mapping (CRTC tick/line conversion) is delegated
 * to a weak gpu_query_light_gun_position() shim; until gpu.c defines it the
 * controller answers "out of range" and games behave as if the gun is aimed
 * off-screen.  This is enough for the SIO protocol to step correctly without
 * needing the full gpu.c rewrite.
 */

#include "core/guncon.h"
#include "core/gpu.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  GC_TRANSFER_IDLE = 0,
  GC_TRANSFER_READY,
  GC_TRANSFER_ID_MSB,
  GC_TRANSFER_BUTTONS_LSB,
  GC_TRANSFER_BUTTONS_MSB,
  GC_TRANSFER_X_LSB,
  GC_TRANSFER_X_MSB,
  GC_TRANSFER_Y_LSB,
  GC_TRANSFER_Y_MSB,
} gc_transfer_state_t;

typedef struct {
  controller_t base;

  s16 tick_offset;
  s8  line_offset;
  bool has_relative_binds;
  float x_scale;

  u16 button_state;     /* active low */
  u16 position_x;
  u16 position_y;
  bool shoot_offscreen;

  gc_transfer_state_t transfer_state;

  float relative_pos[4];  /* L, R, U, D */
  u8    cursor_index;
} guncon_t;

static const controller_vtable_t s_gc_vtable;

/* Bit positions (in PS1 button word) for trigger/A/B. */
static const u8 s_button_bits[GUNCON_BIND_BUTTON_COUNT - 1] = { 13, 3, 14 };

#define GC_DEFAULT_LINE_OFFSET   0
#define GC_DEFAULT_TICK_OFFSET   (-140)

static inline int clampi(int v, int lo, int hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

controller_t* guncon_create(u32 index)
{
  guncon_t* gc = (guncon_t*)calloc(1, sizeof(*gc));
  if (!gc) return NULL;
  gc->base.vtbl   = &s_gc_vtable;
  gc->base.index  = index;
  gc->button_state = 0xFFFFu;
  gc->tick_offset  = GC_DEFAULT_TICK_OFFSET;
  gc->line_offset  = GC_DEFAULT_LINE_OFFSET;
  gc->x_scale      = 1.0f;
  /* Frontend always feeds the gun via the Relative half-axis path, so default
   * to true.  load_settings() may flip this off if the user binds an absolute
   * pointer instead. */
  gc->has_relative_binds = true;
  return &gc->base;
}

static void gc_destroy(controller_t* base) { free(base); }

static controller_type_t gc_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_GUNCON;
}

static void gc_reset(controller_t* base)
{
  ((guncon_t*)base)->transfer_state = GC_TRANSFER_IDLE;
}

static void gc_reset_transfer_state(controller_t* base)
{
  ((guncon_t*)base)->transfer_state = GC_TRANSFER_IDLE;
}

static bool gc_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  guncon_t* gc = (guncon_t*)base;
  u16 button_state = gc->button_state;
  u16 position_x   = gc->position_x;
  u16 position_y   = gc->position_y;
  state_wrapper_do_u16(sw, &button_state);
  state_wrapper_do_u16(sw, &position_x);
  state_wrapper_do_u16(sw, &position_y);
  if (apply_input_state) {
    gc->button_state = button_state;
    gc->position_x   = position_x;
    gc->position_y   = position_y;
  }
  u8 ts = (u8)gc->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  gc->transfer_state = (gc_transfer_state_t)ts;
  return true;
}

static float gc_get_bind_state(const controller_t* base, u32 index)
{
  const guncon_t* gc = (const guncon_t*)base;
  if (index >= sizeof(s_button_bits) / sizeof(s_button_bits[0]))
    return 0.0f;
  const u32 bit = s_button_bits[index];
  return (float)(((gc->button_state >> bit) & 1u) ^ 1u);
}

static void gc_set_bind_state(controller_t* base, u32 index, float value)
{
  guncon_t* gc = (guncon_t*)base;
  const bool pressed = (value >= 0.5f);
  if (index == (u32)GUNCON_BIND_SHOOT_OFFSCREEN) {
    if (gc->shoot_offscreen != pressed) {
      gc->shoot_offscreen = pressed;
      gc_set_bind_state(base, (u32)GUNCON_BIND_TRIGGER, pressed ? 1.0f : 0.0f);
    }
    return;
  }
  if (index >= (u32)GUNCON_BIND_BUTTON_COUNT) {
    if (index >= (u32)GUNCON_BIND_COUNT || !gc->has_relative_binds)
      return;
    const u32 sub = index - (u32)GUNCON_BIND_RELATIVE_LEFT;
    if (gc->relative_pos[sub] != value)
      gc->relative_pos[sub] = value;
    return;
  }

  const u16 bit = (u16)(1u << s_button_bits[index]);
  if (pressed) gc->button_state &= (u16)~bit;
  else         gc->button_state |= bit;
}

/* Resolve the gun's window-relative position to (0..1, 0..1) when the user
 * has bound relative axes to a stick; otherwise we just feed the absolute
 * pointer 0,0 (which gpu_query_light_gun_position() should treat as
 * "centred" once it exists). */
static void gc_get_normalized_pointer(const guncon_t* gc, float* out_x, float* out_y)
{
  if (gc->has_relative_binds) {
    *out_x = (((gc->relative_pos[1] > 0.0f) ? gc->relative_pos[1] : -gc->relative_pos[0]) + 1.0f) * 0.5f;
    *out_y = (((gc->relative_pos[3] > 0.0f) ? gc->relative_pos[3] : -gc->relative_pos[2]) + 1.0f) * 0.5f;
  } else {
    *out_x = 0.0f;
    *out_y = 0.0f;
  }
}

static void gc_update_position(guncon_t* gc)
{
  if (gc->shoot_offscreen) {
    gc->position_x = 0x01;
    gc->position_y = 0x0A;
    return;
  }

  float px, py;
  gc_get_normalized_pointer(gc, &px, &py);
  u16 raw_x = 0, raw_y = 0;
  if (!gpu_query_light_gun_position(gc->base.index, px, py, gc->x_scale, &raw_x, &raw_y)) {
    DEV_LOG("Lightgun out of range");
    gc->position_x = 0x01;
    gc->position_y = 0x0A;
    return;
  }

  const int offset_tick = (int)raw_x + (int)gc->tick_offset;
  const int offset_line = (int)raw_y + (int)gc->line_offset;
  if (offset_tick < 0 || offset_line < 0) {
    gc->position_x = 0x01;
    gc->position_y = 0x0A;
    return;
  }

  /* GunCon reports its X in 8 MHz units.  CRTC frequency varies between
   * NTSC/PAL/overclock, so divide raw CRTC ticks by (CRTC freq / 8 MHz). */
  const tick_count_t freq = gpu_get_crtc_frequency();
  const float divider = (freq > 0) ? ((float)freq / 8000000.0f) : 1.0f;
  const float scaled_x = (float)offset_tick / divider;
  gc->position_x = (u16)scaled_x;
  gc->position_y = (u16)offset_line;
}

static bool gc_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  guncon_t* gc = (guncon_t*)base;
  static const u16 ID = 0x5A63u;

  switch (gc->transfer_state) {
    case GC_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) { gc->transfer_state = GC_TRANSFER_READY; return true; }
      return false;

    case GC_TRANSFER_READY:
      if (data_in == 0x42) {
        *data_out = (u8)ID;
        gc->transfer_state = GC_TRANSFER_ID_MSB;
        return true;
      }
      *data_out = 0xFF;
      return false;

    case GC_TRANSFER_ID_MSB:
      *data_out = (u8)(ID >> 8);
      gc->transfer_state = GC_TRANSFER_BUTTONS_LSB;
      return true;

    case GC_TRANSFER_BUTTONS_LSB:
      *data_out = (u8)gc->button_state;
      gc->transfer_state = GC_TRANSFER_BUTTONS_MSB;
      return true;

    case GC_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)(gc->button_state >> 8);
      gc->transfer_state = GC_TRANSFER_X_LSB;
      return true;

    case GC_TRANSFER_X_LSB:
      gc_update_position(gc);
      *data_out = (u8)gc->position_x;
      gc->transfer_state = GC_TRANSFER_X_MSB;
      return true;

    case GC_TRANSFER_X_MSB:
      *data_out = (u8)(gc->position_x >> 8);
      gc->transfer_state = GC_TRANSFER_Y_LSB;
      return true;

    case GC_TRANSFER_Y_LSB:
      *data_out = (u8)gc->position_y;
      gc->transfer_state = GC_TRANSFER_Y_MSB;
      return true;

    case GC_TRANSFER_Y_MSB:
      *data_out = (u8)(gc->position_y >> 8);
      gc->transfer_state = GC_TRANSFER_IDLE;
      return false;
  }
  return false;
}

static u32 gc_get_button_state_bits(const controller_t* base)
{
  const guncon_t* gc = (const guncon_t*)base;
  return (u32)(gc->button_state ^ 0xFFFFu);
}

static void gc_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  guncon_t* gc = (guncon_t*)base;
  gc->x_scale = settings_interface_get_float_value(si, section, "XScale", 1.0f);

  gc->has_relative_binds =
      settings_interface_contains_value(si, section, "RelativeLeft")  ||
      settings_interface_contains_value(si, section, "RelativeRight") ||
      settings_interface_contains_value(si, section, "RelativeUp")    ||
      settings_interface_contains_value(si, section, "RelativeDown");

   gc->line_offset = (s8)clampi(
      (int)settings_interface_get_int_value(si, section, "GunConLineOffset", GC_DEFAULT_LINE_OFFSET), 
      -128, 127);
   gc->tick_offset = (s16)clampi(
      (int)settings_interface_get_int_value(si, section, "GunConTickOffset", GC_DEFAULT_TICK_OFFSET), 
      -32768, 32767);
}

#define BTN(label, idx, gen)                                                       \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                   \
    .generic_mapping = (gen) }
#define HAXIS(label, idx, gen)                                                     \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_HALF_AXIS,                \
    .generic_mapping = (gen) }

static const controller_binding_info_t s_gc_bindings[] = {
  { .name = "Pointer", .display_name = "Pointer/Aiming", .icon_name = NULL,
     .bind_index = (u32)GUNCON_BIND_BUTTON_COUNT, .type = INPUT_BINDING_TYPE_POINTER,
    .generic_mapping = GENERIC_INPUT_BINDING_UNKNOWN }, 
  BTN("Trigger",        GUNCON_BIND_TRIGGER,         GENERIC_INPUT_BINDING_R2),
  BTN("ShootOffscreen", GUNCON_BIND_SHOOT_OFFSCREEN, GENERIC_INPUT_BINDING_L2),
  BTN("A",              GUNCON_BIND_A,               GENERIC_INPUT_BINDING_CROSS),
  BTN("B",              GUNCON_BIND_B,               GENERIC_INPUT_BINDING_CIRCLE),

  HAXIS("RelativeLeft",  GUNCON_BIND_RELATIVE_LEFT,  GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeRight", GUNCON_BIND_RELATIVE_RIGHT, GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeUp",    GUNCON_BIND_RELATIVE_UP,    GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeDown",  GUNCON_BIND_RELATIVE_DOWN,  GENERIC_INPUT_BINDING_UNKNOWN),
};

#undef BTN
#undef HAXIS

const controller_info_t g_guncon_info = {
   .type           = CONTROLLER_TYPE_GUNCON,
  .name           = "GunCon",
  .display_name   = "GunCon",
  .icon_name      = NULL,
  .bindings       = s_gc_bindings,
  .bindings_count = sizeof(s_gc_bindings) / sizeof(s_gc_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_gc_vtable = {
   .destroy                = gc_destroy,
  .get_type               = gc_get_type,
  .reset                  = gc_reset,
  .do_state               = gc_do_state,
  .reset_transfer_state   = gc_reset_transfer_state,
  .transfer               = gc_transfer,
  .get_bind_state         = gc_get_bind_state,
  .set_bind_state         = gc_set_bind_state,
  .get_button_state_bits  = gc_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = gc_load_settings, 
};
