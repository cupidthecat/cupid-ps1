#include "core/negcon.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  NG_TRANSFER_IDLE = 0,
  NG_TRANSFER_READY,
  NG_TRANSFER_ID_MSB,
  NG_TRANSFER_BUTTONS_LSB,
  NG_TRANSFER_BUTTONS_MSB,
  NG_TRANSFER_ANALOG_STEERING,
  NG_TRANSFER_ANALOG_I,
  NG_TRANSFER_ANALOG_II,
  NG_TRANSFER_ANALOG_L,
} ng_transfer_state_t;

typedef struct {
  float deadzone;
  float saturation;
  float linearity;
  float scaling;
  float zero;
  float unit;
} ng_axis_modifier_t;

typedef struct {
  controller_t base;

  u8  axis_state[NEGCON_AXIS_COUNT];
  float half_axis_state[2];   /* steering left, steering right */
  u16 button_state;

  ng_transfer_state_t transfer_state;

  ng_axis_modifier_t steering_modifier;
  ng_axis_modifier_t pedal_modifiers[3];   /* I, II, L */
} negcon_t;

/* Bit positions in the standard PS1 button word for each NeGcon button. */
static const u8 s_button_bits[NEGCON_BUTTON_COUNT] = {
  /* Start */ 3, /* Up */ 4, /* Right */ 5, /* Down */ 6,
  /* Left  */ 7, /* R  */ 11, /* B    */ 12, /* A    */ 13,
};

#define NG_DEFAULT_DEADZONE    0.0f
#define NG_DEFAULT_SATURATION  1.0f
#define NG_DEFAULT_LINEARITY   0.0f
#define NG_DEFAULT_SCALING     1.0f
#define NG_STEERING_ZERO       128.0f
#define NG_STEERING_UNIT       128.0f
#define NG_PEDAL_ZERO          0.0f
#define NG_PEDAL_UNIT          255.0f

static const ng_axis_modifier_t NG_DEFAULT_STEERING = {
  NG_DEFAULT_DEADZONE, NG_DEFAULT_SATURATION, NG_DEFAULT_LINEARITY,
  NG_DEFAULT_SCALING,  NG_STEERING_ZERO,      NG_STEERING_UNIT,
};
static const ng_axis_modifier_t NG_DEFAULT_PEDAL = {
  NG_DEFAULT_DEADZONE, NG_DEFAULT_SATURATION, NG_DEFAULT_LINEARITY,
  NG_DEFAULT_SCALING,  NG_PEDAL_ZERO,         NG_PEDAL_UNIT,
};

static const controller_vtable_t s_ng_vtable;

static inline float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

static float ng_apply_modifier(float v, const ng_axis_modifier_t* m)
{
  v = (v - m->deadzone) / (m->saturation - m->deadzone);
  v = clampf(v, 0.0f, 1.0f);
  v = powf(v, expf(m->linearity));
  return v;
}

static u8 ng_scale(float v, const ng_axis_modifier_t* m)
{
  v = m->scaling * m->unit * v + m->zero;
  return (u8)clampf(roundf(v), 0.0f, 255.0f);
}

controller_t* negcon_create(u32 index)
{
  negcon_t* n = (negcon_t*)calloc(1, sizeof(*n));
  if (!n) return NULL;
  n->base.vtbl  = &s_ng_vtable;
  n->base.index = index;
  n->axis_state[NEGCON_AXIS_STEERING] = 0x80;
  n->button_state = 0xFFFFu;
  n->steering_modifier  = NG_DEFAULT_STEERING;
  n->pedal_modifiers[0] = NG_DEFAULT_PEDAL;
  n->pedal_modifiers[1] = NG_DEFAULT_PEDAL;
  n->pedal_modifiers[2] = NG_DEFAULT_PEDAL;
  return &n->base;
}

static void ng_destroy(controller_t* base) { free(base); }

static controller_type_t ng_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_NEGCON;
}

static void ng_reset(controller_t* base)
{
  ((negcon_t*)base)->transfer_state = NG_TRANSFER_IDLE;
}

static void ng_reset_transfer_state(controller_t* base)
{
  ((negcon_t*)base)->transfer_state = NG_TRANSFER_IDLE;
}

static bool ng_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  negcon_t* n = (negcon_t*)base;
  u16 button_state = n->button_state;
  state_wrapper_do_u16(sw, &button_state);
  if (apply_input_state) n->button_state = button_state;
  u8 ts = (u8)n->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  n->transfer_state = (ng_transfer_state_t)ts;
  return true;
}

static float ng_get_bind_state(const controller_t* base, u32 index)
{
  const negcon_t* n = (const negcon_t*)base;
  const u32 left_idx  = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_LEFT;
  const u32 right_idx = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_RIGHT;

  if (index == left_idx || index == right_idx) {
    float value = (float)n->axis_state[NEGCON_AXIS_STEERING] - 128.0f;
    value /= (value < 0.0f) ? 128.0f : 127.0f;
    value = clampf(value, -1.0f, 1.0f);
    if (index == left_idx) value = -value;
    return (value > 0.0f) ? value : 0.0f;
  }
  if (index >= (u32)NEGCON_BUTTON_COUNT) {
    /* skip the two steering halves to map onto axis_state */
    const u32 sub = index - ((u32)NEGCON_BUTTON_COUNT + 1u);
    if (sub >= NEGCON_AXIS_COUNT) return 0.0f;
    return (float)n->axis_state[sub] * (1.0f / 255.0f);
  }
  if (index < (u32)NEGCON_BUTTON_COUNT) {
    const u32 bit = s_button_bits[index];
    return (float)(((n->button_state >> bit) & 1u) ^ 1u);
  }
  return 0.0f;
}

static void ng_set_bind_state(controller_t* base, u32 index, float value)
{
  negcon_t* n = (negcon_t*)base;
  const u32 left_idx  = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_LEFT;
  const u32 right_idx = (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_STEERING_RIGHT;

  if (index == left_idx || index == right_idx) {
    value = ng_apply_modifier(value, &n->steering_modifier);
    n->half_axis_state[index - (u32)NEGCON_BUTTON_COUNT] = clampf(value, 0.0f, 1.0f);
    const float merged = n->half_axis_state[1] - n->half_axis_state[0];
    n->axis_state[NEGCON_AXIS_STEERING] = ng_scale(merged, &n->steering_modifier);
    return;
  }
  if (index >= (u32)NEGCON_BUTTON_COUNT) {
    const u32 sub = index - ((u32)NEGCON_BUTTON_COUNT + 1u);
    if (sub >= NEGCON_AXIS_COUNT) return;
    if (index >= (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_I &&
        index <= (u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_L) {
      const u32 mod_idx = index - ((u32)NEGCON_BUTTON_COUNT + (u32)NEGCON_HALFAXIS_I);
      const ng_axis_modifier_t* m = &n->pedal_modifiers[mod_idx];
      value = ng_apply_modifier(value, m);
      n->axis_state[sub] = ng_scale(value, m);
    } else {
      n->axis_state[sub] = (u8)clampf(value * 255.0f, 0.0f, 255.0f);
    }
    return;
  }
  if (index < (u32)NEGCON_BUTTON_COUNT) {
    const u16 bit = (u16)(1u << s_button_bits[index]);
    if (value >= 0.5f) n->button_state &= (u16)~bit;
    else               n->button_state |= bit;
  }
}

static u32 ng_get_button_state_bits(const controller_t* base)
{
  return (u32)(((const negcon_t*)base)->button_state ^ 0xFFFFu);
}

static bool ng_get_analog_input_bytes(const controller_t* base, u32* out_bytes)
{
  const negcon_t* n = (const negcon_t*)base;
  *out_bytes = ((u32)n->axis_state[NEGCON_AXIS_L]        << 24) |
               ((u32)n->axis_state[NEGCON_AXIS_II]       << 16) |
               ((u32)n->axis_state[NEGCON_AXIS_I]        <<  8) | 
                (u32)n->axis_state[NEGCON_AXIS_STEERING];
  return true;
}

static bool ng_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  negcon_t* n = (negcon_t*)base;
  static const u16 ID = 0x5A23u;

  switch (n->transfer_state) {
    case NG_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) { n->transfer_state = NG_TRANSFER_READY; return true; }
      return false;
    case NG_TRANSFER_READY:
      if (data_in == 0x42) { *data_out = (u8)ID; n->transfer_state = NG_TRANSFER_ID_MSB; return true; }
      *data_out = 0xFF;
      return false;
    case NG_TRANSFER_ID_MSB:
      *data_out = (u8)(ID >> 8);
      n->transfer_state = NG_TRANSFER_BUTTONS_LSB;
      return true;
    case NG_TRANSFER_BUTTONS_LSB:
      *data_out = (u8)n->button_state;
      n->transfer_state = NG_TRANSFER_BUTTONS_MSB;
      return true;
    case NG_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)(n->button_state >> 8);
      n->transfer_state = NG_TRANSFER_ANALOG_STEERING;
      return true;
    case NG_TRANSFER_ANALOG_STEERING:
      *data_out = n->axis_state[NEGCON_AXIS_STEERING];
      n->transfer_state = NG_TRANSFER_ANALOG_I;
      return true;
    case NG_TRANSFER_ANALOG_I:
      *data_out = n->axis_state[NEGCON_AXIS_I];
      n->transfer_state = NG_TRANSFER_ANALOG_II;
      return true;
    case NG_TRANSFER_ANALOG_II:
      *data_out = n->axis_state[NEGCON_AXIS_II];
      n->transfer_state = NG_TRANSFER_ANALOG_L;
      return true;
    case NG_TRANSFER_ANALOG_L:
      *data_out = n->axis_state[NEGCON_AXIS_L];
      n->transfer_state = NG_TRANSFER_IDLE;
      return false;
  }
  return false;
}

static void load_modifier(const settings_interface_t* si, const char* section,
                          const char* prefix, ng_axis_modifier_t* m,
                          const ng_axis_modifier_t* def)
{
  char key[64];
  m->zero = def->zero;
  m->unit = def->unit;
  snprintf(key, sizeof(key), "%sDeadzone",   prefix);
  m->deadzone   = settings_interface_get_float_value(si, section, key, def->deadzone);
  snprintf(key, sizeof(key), "%sSaturation", prefix);
  m->saturation = settings_interface_get_float_value(si, section, key, def->saturation);
  snprintf(key, sizeof(key), "%sLinearity",  prefix);
  m->linearity  = settings_interface_get_float_value(si, section, key, def->linearity);
  snprintf(key, sizeof(key), "%sScaling",    prefix);
  m->scaling    = settings_interface_get_float_value(si, section, key, def->scaling);
}

static void ng_load_settings(controller_t* base, const settings_interface_t* si,
                             const char* section, bool initial)
{
  (void)initial;
  negcon_t* n = (negcon_t*)base;
  load_modifier(si, section, "Steering", &n->steering_modifier,  &NG_DEFAULT_STEERING);
  load_modifier(si, section, "I",        &n->pedal_modifiers[0], &NG_DEFAULT_PEDAL);
  load_modifier(si, section, "II",       &n->pedal_modifiers[1], &NG_DEFAULT_PEDAL);
  load_modifier(si, section, "L",        &n->pedal_modifiers[2], &NG_DEFAULT_PEDAL);
}

#define BTN(label, idx, gen)                                                              \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = (u32)(idx), .type = INPUT_BINDING_TYPE_BUTTON,                          \
    .generic_mapping = (gen) }
#define AXIS(label, idx, gen)                                                             \
  { .name = label, .display_name = label, .icon_name = NULL,                              \
    .bind_index = (u32)NEGCON_BUTTON_COUNT + (u32)(idx),                                  \
    .type = INPUT_BINDING_TYPE_HALF_AXIS, .generic_mapping = (gen) }

static const controller_binding_info_t s_ng_bindings[] = {
  BTN("Up",            NEGCON_BUTTON_UP,             GENERIC_INPUT_BINDING_DPAD_UP),
  BTN("Right",         NEGCON_BUTTON_RIGHT,          GENERIC_INPUT_BINDING_DPAD_RIGHT),
  BTN("Down",          NEGCON_BUTTON_DOWN,           GENERIC_INPUT_BINDING_DPAD_DOWN),
  BTN("Left",          NEGCON_BUTTON_LEFT,           GENERIC_INPUT_BINDING_DPAD_LEFT),
  BTN("Start",         NEGCON_BUTTON_START,          GENERIC_INPUT_BINDING_START),
  BTN("A",             NEGCON_BUTTON_A,              GENERIC_INPUT_BINDING_CIRCLE),
  BTN("B",             NEGCON_BUTTON_B,              GENERIC_INPUT_BINDING_TRIANGLE),
  AXIS("I",            NEGCON_HALFAXIS_I,            GENERIC_INPUT_BINDING_R2),
  AXIS("II",           NEGCON_HALFAXIS_II,           GENERIC_INPUT_BINDING_L2),
  AXIS("L",            NEGCON_HALFAXIS_L,            GENERIC_INPUT_BINDING_L1),
  BTN("R",             NEGCON_BUTTON_R,              GENERIC_INPUT_BINDING_R1),
  AXIS("SteeringLeft", NEGCON_HALFAXIS_STEERING_LEFT,  GENERIC_INPUT_BINDING_LEFT_STICK_LEFT),
  AXIS("SteeringRight",NEGCON_HALFAXIS_STEERING_RIGHT, GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT),
};

#undef BTN
#undef AXIS

const controller_info_t g_negcon_info = {
   .type           = CONTROLLER_TYPE_NEGCON,
  .name           = "NeGcon",
  .display_name   = "NeGcon",
  .icon_name      = NULL,
  .bindings       = s_ng_bindings,
  .bindings_count = sizeof(s_ng_bindings) / sizeof(s_ng_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_ng_vtable = {
   .destroy                = ng_destroy,
  .get_type               = ng_get_type,
  .reset                  = ng_reset,
  .do_state               = ng_do_state,
  .reset_transfer_state   = ng_reset_transfer_state,
  .transfer               = ng_transfer,
  .get_bind_state         = ng_get_bind_state,
  .set_bind_state         = ng_set_bind_state,
  .get_button_state_bits  = ng_get_button_state_bits,
  .get_analog_input_bytes = ng_get_analog_input_bytes,
  .load_settings          = ng_load_settings, 
};
