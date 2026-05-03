#include "core/playstation_mouse.h"
#include "common/log.h"
#include "common/settings_interface.h"
#include "util/state_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(Controller);

typedef enum : u8 {
  PSM_TRANSFER_IDLE = 0,
  PSM_TRANSFER_READY,
  PSM_TRANSFER_ID_MSB,
  PSM_TRANSFER_BUTTONS_LSB,
  PSM_TRANSFER_BUTTONS_MSB,
  PSM_TRANSFER_DELTA_X,
  PSM_TRANSFER_DELTA_Y,
} psm_transfer_state_t;

typedef struct {
  controller_t base;

  float sensitivity_x;
  float sensitivity_y;
  u16   button_state;
  float delta_x;
  float delta_y;
  psm_transfer_state_t transfer_state;
} playstation_mouse_t;

static const controller_vtable_t s_psm_vtable;

/* Bit positions in PS1 button word for the two physical buttons (Mouse maps
 * Left → bit 11 (R1 slot), Right → bit 10 (L1 slot) on the protocol). */
static const u8 s_button_bits[PSMOUSE_BIND_BUTTON_COUNT] = { 11, 10 };

static inline float clampf(float v, float lo, float hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}

controller_t* playstation_mouse_create(u32 index)
{
  playstation_mouse_t* m = (playstation_mouse_t*)calloc(1, sizeof(*m));
  if (!m) return NULL;
  m->base.vtbl    = &s_psm_vtable;
  m->base.index   = index;
  m->sensitivity_x = 1.0f;
  m->sensitivity_y = 1.0f;
  m->button_state  = 0xFFFFu;
  return &m->base;
}

static void psm_destroy(controller_t* base) { free(base); }

static controller_type_t psm_get_type(const controller_t* base)
{
  (void)base;
  return CONTROLLER_TYPE_PLAYSTATION_MOUSE;
}

static void psm_reset(controller_t* base)
{
  ((playstation_mouse_t*)base)->transfer_state = PSM_TRANSFER_IDLE;
}

static void psm_reset_transfer_state(controller_t* base)
{
  ((playstation_mouse_t*)base)->transfer_state = PSM_TRANSFER_IDLE;
}

static bool psm_do_state(controller_t* base, state_wrapper_t* sw, bool apply_input_state)
{
  playstation_mouse_t* m = (playstation_mouse_t*)base;
  u16   button_state = m->button_state;
  float delta_x = m->delta_x;
  float delta_y = m->delta_y;
  state_wrapper_do_u16(sw, &button_state);
  if (state_wrapper_get_version(sw) >= 60u) {
    state_wrapper_do_float(sw, &delta_x);
    state_wrapper_do_float(sw, &delta_y);
  } else {
    u8 dummy = 0;
    state_wrapper_do_u8(sw, &dummy);
    state_wrapper_do_u8(sw, &dummy);
  }
  if (apply_input_state) {
    m->button_state = button_state;
    m->delta_x      = delta_x;
    m->delta_y      = delta_y;
  }
  u8 ts = (u8)m->transfer_state;
  state_wrapper_do_u8(sw, &ts);
  m->transfer_state = (psm_transfer_state_t)ts;
  return true;
}

static float psm_get_bind_state(const controller_t* base, u32 index)
{
  const playstation_mouse_t* m = (const playstation_mouse_t*)base;
  if (index >= PSMOUSE_BIND_BUTTON_COUNT) return 0.0f;
  const u32 bit = s_button_bits[index];
  return (float)(((m->button_state >> bit) & 1u) ^ 1u);
}

static void psm_set_bind_state(controller_t* base, u32 index, float value)
{
  playstation_mouse_t* m = (playstation_mouse_t*)base;
  if (index >= PSMOUSE_BIND_BUTTON_COUNT) {
    if (index == PSMOUSE_BIND_POINTER_X) m->delta_x += value;
    else if (index == PSMOUSE_BIND_POINTER_Y) m->delta_y += value;
    return;
  }
  const u16 bit = (u16)(1u << s_button_bits[index]);
  if (value >= 0.5f) m->button_state &= (u16)~bit;
  else               m->button_state |= bit;
}

static u32 psm_get_button_state_bits(const controller_t* base)
{
  const playstation_mouse_t* m = (const playstation_mouse_t*)base;
  return (u32)(m->button_state ^ 0xFFFFu);
}

static bool psm_transfer(controller_t* base, u8 data_in, u8* data_out)
{
  playstation_mouse_t* m = (playstation_mouse_t*)base;
  static const u16 ID = 0x5A12u;

  switch (m->transfer_state) {
    case PSM_TRANSFER_IDLE:
      *data_out = 0xFF;
      if (data_in == 0x01) { m->transfer_state = PSM_TRANSFER_READY; return true; }
      return false;

    case PSM_TRANSFER_READY:
      if (data_in == 0x42) {
        *data_out = (u8)ID;
        m->transfer_state = PSM_TRANSFER_ID_MSB;
        return true;
      }
      *data_out = 0xFF;
      return false;

    case PSM_TRANSFER_ID_MSB:
      *data_out = (u8)(ID >> 8);
      m->transfer_state = PSM_TRANSFER_BUTTONS_LSB;
      return true;

    case PSM_TRANSFER_BUTTONS_LSB:
      *data_out = (u8)m->button_state;
      m->transfer_state = PSM_TRANSFER_BUTTONS_MSB;
      return true;

    case PSM_TRANSFER_BUTTONS_MSB:
      *data_out = (u8)(m->button_state >> 8);
      m->transfer_state = PSM_TRANSFER_DELTA_X;
      return true;

    case PSM_TRANSFER_DELTA_X: {
      const float delta = floorf(m->delta_x * m->sensitivity_x);
      m->delta_x -= delta / m->sensitivity_x;
      *data_out = (u8)(s8)clampf(delta, -128.0f, 127.0f);
      m->transfer_state = PSM_TRANSFER_DELTA_Y;
      return true;
    }

    case PSM_TRANSFER_DELTA_Y: {
      const float delta = floorf(m->delta_y * m->sensitivity_y);
      m->delta_y -= delta / m->sensitivity_y;
      *data_out = (u8)(s8)clampf(delta, -128.0f, 127.0f);
      m->transfer_state = PSM_TRANSFER_IDLE;
      return false;
    }
  }
  return false;
}

static void psm_load_settings(controller_t* base, const settings_interface_t* si,
                              const char* section, bool initial)
{
  (void)initial;
  playstation_mouse_t* m = (playstation_mouse_t*)base;
  m->sensitivity_x = settings_interface_get_float_value(si, section, "SensitivityX", 1.0f);
  m->sensitivity_y = settings_interface_get_float_value(si, section, "SensitivityY", 1.0f);
}

static const controller_binding_info_t s_psm_bindings[] = {
  { .name = "Pointer", .display_name = "Pointer", .icon_name = NULL,
     .bind_index = PSMOUSE_BIND_POINTER_X, .type = INPUT_BINDING_TYPE_RELATIVE_POINTER,
    .generic_mapping = GENERIC_INPUT_BINDING_UNKNOWN }, 
  { .name = "Left",  .display_name = "Left Button",  .icon_name = NULL,
     .bind_index = PSMOUSE_BIND_LEFT,  .type = INPUT_BINDING_TYPE_BUTTON,
    .generic_mapping = GENERIC_INPUT_BINDING_CROSS }, 
  { .name = "Right", .display_name = "Right Button", .icon_name = NULL,
     .bind_index = PSMOUSE_BIND_RIGHT, .type = INPUT_BINDING_TYPE_BUTTON,
    .generic_mapping = GENERIC_INPUT_BINDING_CIRCLE }, 
};

const controller_info_t g_playstation_mouse_info = {
   .type           = CONTROLLER_TYPE_PLAYSTATION_MOUSE,
  .name           = "PlayStationMouse",
  .display_name   = "Mouse",
  .icon_name      = NULL,
  .bindings       = s_psm_bindings,
  .bindings_count = sizeof(s_psm_bindings) / sizeof(s_psm_bindings[0]),
  .settings       = NULL,
  .settings_count = 0, 
};

static const controller_vtable_t s_psm_vtable = {
   .destroy                = psm_destroy,
  .get_type               = psm_get_type,
  .reset                  = psm_reset,
  .do_state               = psm_do_state,
  .reset_transfer_state   = psm_reset_transfer_state,
  .transfer               = psm_transfer,
  .get_bind_state         = psm_get_bind_state,
  .set_bind_state         = psm_set_bind_state,
  .get_button_state_bits  = psm_get_button_state_bits,
  .get_analog_input_bytes = NULL,
  .load_settings          = psm_load_settings, 
};
