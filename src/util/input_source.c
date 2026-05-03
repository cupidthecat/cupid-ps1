#include "input_source.h"

#include "common/log.h"
#include "common/string_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(InputManager);

void input_device_list_free(input_device_list_t* list)
{
  if (!list || !list->items)
    return;
  for (size_t i = 0; i < list->count; i++) {
    free(list->items[i].identifier);
    free(list->items[i].display_name);
  }
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

void input_effect_list_free(input_effect_list_t* list)
{
  if (!list || !list->items)
    return;
  free(list->items);
  list->items = NULL;
  list->count = 0;
}

void generic_binding_mapping_init(generic_binding_mapping_t* m)
{
  m->items    = NULL;
  m->count    = 0;
  m->capacity = 0;
}

void generic_binding_mapping_destroy(generic_binding_mapping_t* m)
{
  if (!m || !m->items)
    return;
  for (size_t i = 0; i < m->count; i++)
    free(m->items[i].binding);
  free(m->items);
  m->items    = NULL;
  m->count    = 0;
  m->capacity = 0;
}

void generic_binding_mapping_push(generic_binding_mapping_t* m, u8 generic, const char* binding)
{
  if (m->count == m->capacity) {
    /* Grow geometrically; start at 8 to avoid the 1->2->3... churn. */
    size_t newcap = m->capacity ? m->capacity * 2 : 8;
    generic_binding_entry_t* p = (generic_binding_entry_t*)realloc(m->items, newcap * sizeof(*p));
    if (!p)
      return;
    m->items    = p;
    m->capacity = newcap;
  }
  m->items[m->count].generic = generic;
  m->items[m->count].binding = binding ? strdup(binding) : NULL;
  m->count++;
}

input_binding_key_t input_source_make_generic_device_key(input_source_type_t clazz, u32 controller_index)
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = (u32)clazz;
  k.bits_.source_index   = controller_index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_NONE;
  k.bits_.data           = 0;
  return k;
}

 input_binding_key_t input_source_make_generic_axis_key(input_source_type_t clazz, u32 controller_index,
                                                       s32 axis_index) 
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = (u32)clazz;
  k.bits_.source_index   = controller_index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
  k.bits_.data           = (u32)axis_index;
  return k;
}

 input_binding_key_t input_source_make_generic_button_key(input_source_type_t clazz, u32 controller_index,
                                                         s32 button_index) 
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = (u32)clazz;
  k.bits_.source_index   = controller_index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_BUTTON;
  k.bits_.data           = (u32)button_index;
  return k;
}

 input_binding_key_t input_source_make_generic_hat_key(input_source_type_t clazz, u32 controller_index,
                                                      s32 hat_index, u8 hat_direction, u32 num_directions) 
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = (u32)clazz;
  k.bits_.source_index   = controller_index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_HAT;
  k.bits_.data           = (u32)hat_index * num_directions + hat_direction;
  return k;
}

 input_binding_key_t input_source_make_generic_motor_key(input_source_type_t clazz, u32 controller_index,
                                                        s32 motor_index) 
{
  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type    = (u32)clazz;
  k.bits_.source_index   = controller_index & 0xFFu;
  k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_MOTOR;
  k.bits_.data           = (u32)motor_index;
  return k;
}

bool input_source_parse_generic_controller_key(input_source_type_t clazz, const char* source,
                                               const char* sub_binding, input_binding_key_t* out_key)
{
  if (!source || !sub_binding)
    return false;

  /* Skip alpha prefix and any '-', then parse the number. */
  size_t pos = 0;
  size_t slen = strlen(source);
  while (pos < slen && !(source[pos] >= '0' && source[pos] <= '9'))
    pos++;
  if (pos == slen)
    return false;

  s32 source_index = 0;
  if (!string_util_from_chars_s32(source + pos, (u32)(slen - pos), 10, &source_index, NULL) ||
      source_index < 0) {
    return false;
  }

  input_binding_key_t k = { .bits = 0 };
  k.bits_.source_type  = (u32)clazz;
  k.bits_.source_index = (u32)source_index & 0xFFu;

  size_t sub_len = strlen(sub_binding);

  if (sub_len >= 5 && (sub_binding[0] == '+' || sub_binding[0] == '-') &&
      memcmp(sub_binding + 1, "Axis", 4) == 0) {
    s32 axis = 0;
    if (!string_util_from_chars_s32(sub_binding + 5, (u32)(sub_len - 5), 10, &axis, NULL) || axis < 0)
      return false;
    k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
    k.bits_.data           = (u32)axis;
    k.bits_.modifier       = (sub_binding[0] == '-') ? INPUT_MODIFIER_NEGATE : INPUT_MODIFIER_NONE;
    *out_key = k;
    return true;
  }

  if (sub_len >= 8 && memcmp(sub_binding, "FullAxis", 8) == 0) {
    s32 axis = 0;
    if (!string_util_from_chars_s32(sub_binding + 8, (u32)(sub_len - 8), 10, &axis, NULL) || axis < 0)
      return false;
    k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_AXIS;
    k.bits_.data           = (u32)axis;
    k.bits_.modifier       = INPUT_MODIFIER_FULL_AXIS;
    *out_key = k;
    return true;
  }

  if (sub_len >= 6 && memcmp(sub_binding, "Button", 6) == 0) {
    s32 btn = 0;
    if (!string_util_from_chars_s32(sub_binding + 6, (u32)(sub_len - 6), 10, &btn, NULL) || btn < 0)
      return false;
    k.bits_.source_subtype = INPUT_SUBCLASS_CONTROLLER_BUTTON;
    k.bits_.data           = (u32)btn;
    *out_key = k;
    return true;
  }

  return false;
}

bool input_source_stub_reload_devices(input_source_t* self)
{
  (void)self;
  return false;
}

bool input_source_stub_get_current_value(input_source_t* self, input_binding_key_t key, float* v)
{
  (void)self; (void)key; (void)v;
  return false;
}

void input_source_stub_update_motor_state(input_source_t* self, input_binding_key_t key, float intensity)
{
  (void)self; (void)key; (void)intensity;
}

void input_source_stub_update_motor_state_pair(input_source_t* self, input_binding_key_t lk,
                                               input_binding_key_t sk, float li, float si_)
{
  if (lk.bits != 0)
    self->vtbl->update_motor_state(self, lk, li);
  if (sk.bits != 0)
    self->vtbl->update_motor_state(self, sk, si_);
}

void input_source_stub_update_led_state(input_source_t* self, input_binding_key_t key, float intensity)
{
  (void)self; (void)key; (void)intensity;
}

bool input_source_stub_get_generic_binding_mapping(input_source_t* self, const char* device,
                                                   generic_binding_mapping_t* mapping)
{
  (void)self; (void)device; (void)mapping;
  return false;
}

void input_source_stub_enumerate_effects(input_source_t* self, int type, bool has_for_device,
                                         input_binding_key_t for_device, input_effect_list_t* out)
{
  (void)self; (void)type; (void)has_for_device; (void)for_device;
  out->items = NULL;
  out->count = 0;
}
