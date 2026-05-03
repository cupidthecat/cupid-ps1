#ifndef CUPID_CORE_INPUT_TYPES_H
#define CUPID_CORE_INPUT_TYPES_H

#include "common/types.h"
#include "core/types.h"

/* Forward decl: settings_interface lives in common/.  We only reference it by
 * pointer in setting_info_copy_value(); the header is not pulled in to keep the
 * include surface minimal. */
typedef struct settings_interface settings_interface_t;

typedef enum : u8 {
  INPUT_BINDING_TYPE_UNKNOWN,
  INPUT_BINDING_TYPE_BUTTON,
  INPUT_BINDING_TYPE_AXIS,
  INPUT_BINDING_TYPE_HALF_AXIS,
  INPUT_BINDING_TYPE_MOTOR,           /* vibration motors; generic_mapping selects motor */
  INPUT_BINDING_TYPE_LED,             /* status LEDs (e.g. analog/digital indicator) */
  INPUT_BINDING_TYPE_POINTER,         /* absolute pointer; queryable, no events */
  INPUT_BINDING_TYPE_RELATIVE_POINTER,/* relative mouse motion; bind_index offset by axis */
  INPUT_BINDING_TYPE_DEVICE,          /* special-purpose device select (e.g. force feedback) */
  INPUT_BINDING_TYPE_MACRO,
} input_binding_type_t;

typedef enum : u8 {
  GENERIC_INPUT_BINDING_UNKNOWN,

  GENERIC_INPUT_BINDING_DPAD_UP,
  GENERIC_INPUT_BINDING_DPAD_RIGHT,
  GENERIC_INPUT_BINDING_DPAD_LEFT,
  GENERIC_INPUT_BINDING_DPAD_DOWN,

  GENERIC_INPUT_BINDING_LEFT_STICK_UP,
  GENERIC_INPUT_BINDING_LEFT_STICK_RIGHT,
  GENERIC_INPUT_BINDING_LEFT_STICK_DOWN,
  GENERIC_INPUT_BINDING_LEFT_STICK_LEFT,
  GENERIC_INPUT_BINDING_L3,

  GENERIC_INPUT_BINDING_RIGHT_STICK_UP,
  GENERIC_INPUT_BINDING_RIGHT_STICK_RIGHT,
  GENERIC_INPUT_BINDING_RIGHT_STICK_DOWN,
  GENERIC_INPUT_BINDING_RIGHT_STICK_LEFT,
  GENERIC_INPUT_BINDING_R3,

  GENERIC_INPUT_BINDING_TRIANGLE, /* Y on Xbox pads */
  GENERIC_INPUT_BINDING_CIRCLE,   /* B on Xbox pads */
  GENERIC_INPUT_BINDING_CROSS,    /* A on Xbox pads */
  GENERIC_INPUT_BINDING_SQUARE,   /* X on Xbox pads */

  GENERIC_INPUT_BINDING_SELECT,   /* Share on DS4, View on Xbox pads */
  GENERIC_INPUT_BINDING_START,    /* Options on DS4, Menu on Xbox pads */
  GENERIC_INPUT_BINDING_SYSTEM,   /* PS button on DS4, Guide on Xbox pads */

  GENERIC_INPUT_BINDING_L1,       /* LB on Xbox */
  GENERIC_INPUT_BINDING_L2,       /* left trigger on Xbox */
  GENERIC_INPUT_BINDING_R1,       /* RB on Xbox */
  GENERIC_INPUT_BINDING_R2,       /* right trigger on Xbox */

  GENERIC_INPUT_BINDING_LARGE_MOTOR, /* low-frequency vibration */
  GENERIC_INPUT_BINDING_SMALL_MOTOR, /* high-frequency vibration */

  GENERIC_INPUT_BINDING_MODE_LED,    /* digital/analog mode indicator */

  GENERIC_INPUT_BINDING_COUNT,
} generic_input_binding_t;

typedef struct {
  const char*             name;
  const char*             display_name;
  input_binding_type_t    bind_type;
  u16                     bind_index;
  generic_input_binding_t generic_mapping;
} input_binding_info_t;

/* Effect types are MOTOR..LED inclusive (the device-feedback subset). */
ALWAYS_INLINE bool input_binding_type_is_effect(input_binding_type_t t)
{
  return t >= INPUT_BINDING_TYPE_MOTOR && t <= INPUT_BINDING_TYPE_LED;
}

typedef enum {
  SETTING_INFO_TYPE_BOOLEAN,
  SETTING_INFO_TYPE_INTEGER,
  SETTING_INFO_TYPE_INTEGER_LIST,
  SETTING_INFO_TYPE_FLOAT,
  SETTING_INFO_TYPE_STRING,
  SETTING_INFO_TYPE_PATH,
} setting_info_type_t;

typedef struct {
  setting_info_type_t type;
  const char*         name;
  const char*         display_name;
  const char*         description;
  const char*         default_value;
  const char*         min_value;
  const char*         max_value;
  const char*         step_value;
  const char*         format;
  const char* const*  options;
  float               multiplier;
} setting_info_t;

/* Implemented in core/input_types.c. */
const char* setting_info_string_default_value(const setting_info_t* si);
bool        setting_info_boolean_default_value(const setting_info_t* si);
s32         setting_info_integer_default_value(const setting_info_t* si);
s32         setting_info_integer_min_value(const setting_info_t* si);
s32         setting_info_integer_max_value(const setting_info_t* si);
s32         setting_info_integer_step_value(const setting_info_t* si);
float       setting_info_float_default_value(const setting_info_t* si);
float       setting_info_float_min_value(const setting_info_t* si);
float       setting_info_float_max_value(const setting_info_t* si);
float       setting_info_float_step_value(const setting_info_t* si);

void setting_info_copy_value(const setting_info_t* si,
                             settings_interface_t* dest_si,
                             const settings_interface_t* src_si,
                             const char* section);

#endif /* CUPID_CORE_INPUT_TYPES_H */
