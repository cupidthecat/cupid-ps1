/*
 * MediaCapture hooks dropped.
 *
 * Light-gun -> framebuffer hit-test is delegated to the weak
 * gpu_query_light_gun_position() symbol declared in guncon.c.  Until gpu.c
 * supplies it, this controller reports the gun as "off-screen" and the IRQ
 * scheduler stays inactive.  The protocol surface (Idle/IDMSB/Buttons*) is
 * still served correctly so games at least step through their boot probe.
 */

#include "core/justifier.h"

#include "core/gpu.h"
#include "core/interrupt_controller.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  JU_TRANSFER_IDLE = 0,
  JU_TRANSFER_ID_MSB,
  JU_TRANSFER_BUTTONS_LSB,
  JU_TRANSFER_BUTTONS_MSB,
} ju_transfer_state_t;

static const char* const s_event_names[NUM_CONTROLLER_AND_CARD_PORTS] = {
  "Justifier IRQ P0", "Justifier IRQ P1", "Justifier IRQ P2", "Justifier IRQ P3",
  "Justifier IRQ P4", "Justifier IRQ P5", "Justifier IRQ P6", "Justifier IRQ P7", 
};

typedef struct {
  controller_t base;

  s8  first_line_offset;
  s8  last_line_offset;
  s16 tick_offset;

  u8  offscreen_oob_frames;
  u8  offscreen_trigger_frames;
  u8  offscreen_release_frames;

  u16 irq_first_line;
  u16 irq_last_line;
  u16 irq_tick;

  u16 button_state;       /* active low */
  u8  shoot_offscreen;
  bool position_valid;
  bool irq_enabled;

  ju_transfer_state_t transfer_state;

  timing_event_t irq_event;

  bool has_relative_binds;
  u8   cursor_index;
  float relative_pos[4];   /* L, R, U, D */

  float x_scale;
} justifier_t;

static const controller_vtable_t s_ju_vtable;

/* PS1 button-word bit positions: trigger 15, start 3, back 14. */
static const u8 s_button_bits[JUSTIFIER_BIND_BUTTON_COUNT - 1] = { 15, 3, 14 };

#define JU_DEFAULT_FIRST_LINE_OFFSET     (-12)
#define JU_DEFAULT_LAST_LINE_OFFSET      (-6)
#define JU_DEFAULT_TICK_OFFSET           50
#define JU_DEFAULT_OFFSCREEN_OOB         5
#define JU_DEFAULT_OFFSCREEN_TRIGGER     5
#define JU_DEFAULT_OFFSCREEN_RELEASE     5

static inline int clampi(int v, int lo, int hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void ju_update_position(justifier_t* ju);
static void ju_update_irq_event(justifier_t* ju);
static void ju_set_bind_state(controller_t* base, u32 index, float value);

static void ju_irq_event_cb(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)ticks; (void)ticks_late;
  /* Pulse IRQ10. */
  interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_IRQ10, true);
  interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_IRQ10, false);
  ju_update_irq_event((justifier_t*)user);
}

controller_t* justifier_create(u32 index)
{
  justifier_t* ju = (justifier_t*)calloc(1, sizeof(*ju));
  if (!ju) return NULL;
  ju->base.vtbl  = &s_ju_vtable;
  ju->base.index = index;
  ju->button_state = 0xFFFFu;
  ju->x_scale      = 1.0f;
  ju->first_line_offset = JU_DEFAULT_FIRST_LINE_OFFSET;
  ju->last_line_offset  = JU_DEFAULT_LAST_LINE_OFFSET;
  ju->tick_offset       = JU_DEFAULT_TICK_OFFSET;
  ju->irq_enabled       = true;
  /* Frontend always feeds the gun via the Relative half-axis path. */
  ju->has_relative_binds = true;

  const char* name = (index < NUM_CONTROLLER_AND_CARD_PORTS) ? s_event_names[index] : "Justifier IRQ";
  timing_event_init(&ju->irq_event, name, (u32)strlen(name), 1, 1, ju_irq_event_cb, ju);
  return &ju->base;
}

static void ju_destroy(controller_t* base)
{
  justifier_t* ju = (justifier_t*)base;
  timing_event_deactivate(&ju->irq_event);
  timing_event_destroy(&ju->irq_event);
  free(ju);
}

static controller_type_t ju_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_JUSTIFIER;
}

static void ju_reset(controller_t* base)
{
  ((justifier_t*)base)->transfer_state = JU_TRANSFER_IDLE;
}

static void ju_reset_transfer_state(controller_t* base)
{
  ((justifier_t*)base)->transfer_state = JU_TRANSFER_IDLE;
}

static bool ju_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  justifier_t* ju = (justifier_t*)base;

  u16 irq_first_line   = ju->irq_first_line;
  u16 irq_last_line    = ju->irq_last_line;
  u16 irq_tick         = ju->irq_tick;
  u16 button_state     = ju->button_state;
  u8  shoot_offscreen  = ju->shoot_offscreen;
  bool position_valid  = ju->position_valid;

  state_wrapper_do_u16 (sw, &irq_first_line);
  state_wrapper_do_u16 (sw, &irq_last_line);
  state_wrapper_do_u16 (sw, &irq_tick);
  state_wrapper_do_u16 (sw, &button_state);
  state_wrapper_do_u8  (sw, &shoot_offscreen);
  state_wrapper_do_bool(sw, &position_valid);

  if (apply_input_state) {
    ju->irq_first_line  = irq_first_line;
    ju->irq_last_line   = irq_last_line;
    ju->irq_tick        = irq_tick;
    ju->button_state    = button_state;
    ju->shoot_offscreen = shoot_offscreen;
    ju->position_valid  = position_valid;
  }

  /* irq_enabled added in save-state v82 (DoEx default=true). Older states
   * lack the byte; gate the read so v42-v81 deserialize correctly. */
  if (state_wrapper_get_version(sw) >= 82u) {
    state_wrapper_do_bool(sw, &ju->irq_enabled);
  } else if (state_wrapper_is_reading(sw)) {
    ju->irq_enabled = true;
  }

  u8 ts = (u8)ju->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  ju->transfer_state = (ju_transfer_state_t)ts;

  return true;
}

static float ju_get_bind_state(const controller_t* base, u32 index)
{
  const justifier_t* ju = (const justifier_t*)base;
  if (index >= sizeof(s_button_bits) / sizeof(s_button_bits[0]))
    return 0.0f;
  const u32 bit = s_button_bits[index];
  return (float)(((ju->button_state >> bit) & 1u) ^ 1u);
}

static void ju_set_bind_state(controller_t* base, u32 index, float value)
{
  justifier_t* ju = (justifier_t*)base;
  const bool pressed = (value >= 0.5f);

  if (index == (u32)JUSTIFIER_BIND_SHOOT_OFFSCREEN) {
    if (pressed)
      ju->shoot_offscreen = ju->shoot_offscreen ? ju->shoot_offscreen : ju->offscreen_oob_frames;
    return;
  }
  if (index >= (u32)JUSTIFIER_BIND_BUTTON_COUNT) {
    if (index >= (u32)JUSTIFIER_BIND_COUNT || !ju->has_relative_binds)
      return;
    const u32 sub = index - (u32)JUSTIFIER_BIND_RELATIVE_LEFT;
    if (ju->relative_pos[sub] != value)
      ju->relative_pos[sub] = value;
    return;
  }

  const u16 bit = (u16)(1u << s_button_bits[index]);
  if (pressed) ju->button_state &= (u16)~bit;
  else         ju->button_state |= bit;
}

static u32 ju_get_button_state_bits(const controller_t* base)
{
  const justifier_t* ju = (const justifier_t*)base;
  return (u32)(ju->button_state ^ 0xFFFFu);
}

static void ju_get_normalized_pointer(const justifier_t* ju, float* out_x, float* out_y)
{
  if (ju->has_relative_binds) {
    *out_x = (((ju->relative_pos[1] > 0.0f) ? ju->relative_pos[1] : -ju->relative_pos[0]) + 1.0f) * 0.5f;
    *out_y = (((ju->relative_pos[3] > 0.0f) ? ju->relative_pos[3] : -ju->relative_pos[2]) + 1.0f) * 0.5f;
  } else {
    *out_x = 0.0f;
    *out_y = 0.0f;
  }
}

static void ju_update_irq_event(justifier_t* ju)
{
  timing_event_deactivate(&ju->irq_event);
  if (!ju->position_valid || !ju->irq_enabled)
    return;

  u32 cur_t, cur_l;
  gpu_get_beam_position(&cur_t, &cur_l);

  u32 target_line;
  if (cur_l < ju->irq_first_line || cur_l >= ju->irq_last_line)
    target_line = ju->irq_first_line;
  else
    target_line = cur_l + 1u;

  const tick_count_t ticks_until_pos =
      gpu_get_system_ticks_until_ticks_and_line((u32)ju->irq_tick, target_line);
  DEV_LOG("Justifier IRQ in %d ticks @ tick %u line %u",
          (int)ticks_until_pos, (u32)ju->irq_tick, target_line);
  timing_event_schedule(&ju->irq_event, ticks_until_pos);
}

static void ju_update_position(justifier_t* ju)
{
  if (ju->shoot_offscreen > 0) {
    if (ju->shoot_offscreen == ju->offscreen_trigger_frames)
      ju_set_bind_state(&ju->base, (u32)JUSTIFIER_BIND_TRIGGER, 1.0f);
    else if (ju->shoot_offscreen == ju->offscreen_release_frames)
      ju_set_bind_state(&ju->base, (u32)JUSTIFIER_BIND_TRIGGER, 0.0f);
    ju->shoot_offscreen--;
    ju->position_valid = false;
    ju_update_irq_event(ju);
    return;
  }

  float px, py;
  ju_get_normalized_pointer(ju, &px, &py);
  u16 raw_tick = 0, raw_line = 0;
  if (!gpu_query_light_gun_position(ju->base.index, px, py, ju->x_scale, &raw_tick, &raw_line)) {
    DEV_LOG("Justifier out of range");
    ju->position_valid = false;
    ju_update_irq_event(ju);
    return;
  }

  ju->position_valid = true;
  ju->irq_tick       = (u16)((int)raw_tick + system_scale_ticks_to_overclock((tick_count_t)ju->tick_offset));
  const int active_start = (int)gpu_get_crtc_active_start_line();
  const int active_end   = (int)gpu_get_crtc_active_end_line();
  ju->irq_first_line = (u16)clampi((int)raw_line + ju->first_line_offset, active_start, active_end);
  ju->irq_last_line  = (u16)clampi((int)raw_line + ju->last_line_offset,  active_start, active_end);
  ju_update_irq_event(ju);
}

static bool ju_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  justifier_t* ju = (justifier_t*)base;
  static const u16 ID = 0x5A31u;

  switch (ju->transfer_state) {
    case JU_TRANSFER_IDLE:
      /* ack on 0x01, send ID for 0x42. */
      if (data_in == 0x42) {
        *data_out = (u8)ID;
        ju->transfer_state = JU_TRANSFER_ID_MSB;
        ju_update_position(ju);
        return true;
      }
      *data_out = 0xFF;
      return (data_in == 0x01);

    case JU_TRANSFER_ID_MSB:
      *data_out = (u8)(ID >> 8);
      ju->transfer_state = JU_TRANSFER_BUTTONS_LSB;
      return true;

    case JU_TRANSFER_BUTTONS_LSB: {
      const bool new_irq_enabled = ((data_in & 0x10) == 0x10);
      if (new_irq_enabled != ju->irq_enabled) {
        ju->irq_enabled = new_irq_enabled;
        ju_update_irq_event(ju);
      }
      *data_out = (u8)ju->button_state;
      ju->transfer_state = JU_TRANSFER_BUTTONS_MSB;
      return true;
    }

    case JU_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)(ju->button_state >> 8);
      ju->transfer_state = JU_TRANSFER_IDLE;
      return true;
  }
  return false;
}

static void ju_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  justifier_t* ju = (justifier_t*)base;
  ju->x_scale = settings_interface_get_float_value(si, section, "XScale", 1.0f);

  ju->has_relative_binds =
      settings_interface_contains_value(si, section, "RelativeLeft")  ||
      settings_interface_contains_value(si, section, "RelativeRight") ||
      settings_interface_contains_value(si, section, "RelativeUp")    ||
      settings_interface_contains_value(si, section, "RelativeDown");

   ju->first_line_offset = (s8)clampi(
      (int)settings_interface_get_int_value(si, section, "FirstLineOffset", JU_DEFAULT_FIRST_LINE_OFFSET), 
      -128, 127);
   ju->last_line_offset = (s8)clampi(
      (int)settings_interface_get_int_value(si, section, "LastLineOffset", JU_DEFAULT_LAST_LINE_OFFSET), 
      -128, 127);
   ju->tick_offset = (s16)clampi(
      (int)settings_interface_get_int_value(si, section, "TickOffset", JU_DEFAULT_TICK_OFFSET), 
      -32768, 32767);

  /* Mirror the weird counter-stacking the C++ does: the saved frame budgets
   * ratchet down as `oob -> trigger -> release` over the lifetime of one
   * shoot-offscreen press. */
  const int oob     = clampi((int)settings_interface_get_int_value(si, section, "OffscreenOOBFrames",     JU_DEFAULT_OFFSCREEN_OOB),     -128, 127);
  const int trigger = clampi((int)settings_interface_get_int_value(si, section, "OffscreenTriggerFrames", JU_DEFAULT_OFFSCREEN_TRIGGER), -128, 127);
  const int release = clampi((int)settings_interface_get_int_value(si, section, "OffscreenReleaseFrames", JU_DEFAULT_OFFSCREEN_RELEASE), -128, 127);
  ju->offscreen_oob_frames     = (u8)(oob + trigger + release);
  ju->offscreen_trigger_frames = (u8)(ju->offscreen_oob_frames - trigger);
  ju->offscreen_release_frames = (u8)(ju->offscreen_trigger_frames - release);
}

#define BTN(label, idx, gen)                                                       \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                   \
    .generic_mapping = (gen) }
#define HAXIS(label, idx, gen)                                                     \
  { .name = label, .display_name = label, .icon_name = NULL,                       \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_HALF_AXIS,                \
    .generic_mapping = (gen) }

static const controller_binding_info_t s_ju_bindings[] = {
  { .name = "Pointer", .display_name = "Pointer/Aiming", .icon_name = NULL,
     .bind_index = (u32)JUSTIFIER_BIND_BUTTON_COUNT, .type = INPUT_BINDING_TYPE_POINTER,
    .generic_mapping = GENERIC_INPUT_BINDING_UNKNOWN }, 
  BTN("Trigger",        JUSTIFIER_BIND_TRIGGER,         GENERIC_INPUT_BINDING_R2),
  BTN("ShootOffscreen", JUSTIFIER_BIND_SHOOT_OFFSCREEN, GENERIC_INPUT_BINDING_L2),
  BTN("Start",          JUSTIFIER_BIND_START,           GENERIC_INPUT_BINDING_CROSS),
  BTN("Back",           JUSTIFIER_BIND_BACK,            GENERIC_INPUT_BINDING_CIRCLE),

  HAXIS("RelativeLeft",  JUSTIFIER_BIND_RELATIVE_LEFT,  GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeRight", JUSTIFIER_BIND_RELATIVE_RIGHT, GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeUp",    JUSTIFIER_BIND_RELATIVE_UP,    GENERIC_INPUT_BINDING_UNKNOWN),
  HAXIS("RelativeDown",  JUSTIFIER_BIND_RELATIVE_DOWN,  GENERIC_INPUT_BINDING_UNKNOWN),
};

#undef BTN
#undef HAXIS

const controller_info_t g_justifier_info = {
   .type           = CONTROLLER_TYPE_JUSTIFIER,
  .name           = "Justifier",
  .display_name   = "Justifier",
  .icon_name      = NULL,
  .bindings       = s_ju_bindings,
  .bindings_count = sizeof(s_ju_bindings) / sizeof(s_ju_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_ju_vtable = {
   .destroy                = ju_destroy,
  .get_type               = ju_get_type,
  .reset                  = ju_reset,
  .do_state               = ju_do_state,
  .reset_transfer_state   = ju_reset_transfer_state,
  .transfer               = ju_transfer,
  .get_bind_state         = ju_get_bind_state,
  .set_bind_state         = ju_set_bind_state,
  .get_button_state_bits  = ju_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = ju_load_settings, 
};
