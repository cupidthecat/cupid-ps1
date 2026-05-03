/*
 * Abstract controller base + registry / factory.  The C++ class hierarchy
 *   Controller (virtual base) <- DigitalController / AnalogController / ...
 * becomes a struct + vtable: each concrete subclass embeds `controller_t` as
 * its first member and supplies its own `controller_vtable_t` filled with
 * function pointers.  A NULL slot means "default"; e.g. a Reset slot left
 * NULL is treated as a no-op, matching the empty C++ default impl.
 *
 * Subclasses dispatch via the inline wrappers (controller_reset, etc.); they
 * take care of the NULL-slot fallback so call sites stay terse.
 *
 * Skipped subclasses (analog_joystick, ddgo, guncon, jogcon, justifier,
 * negcon*, mouse); the boot path only needs digital + analog.  The factory
 * returns NULL for those types instead of asserting.
 */

#ifndef CUPID_CORE_CONTROLLER_H
#define CUPID_CORE_CONTROLLER_H

#include "core/input_types.h"
#include "core/types.h"

#include "common/types.h"

#include <stddef.h>

typedef struct settings_interface settings_interface_t;
typedef struct state_wrapper      state_wrapper_t;
typedef struct controller         controller_t;
typedef struct controller_vtable  controller_vtable_t;

/* Per-binding metadata exposed to the input manager.  The icon_name field
 * is kept (NULL for the headless boot path) so binding tables stay
 * verbatim-compatible. */
typedef struct {
  const char*             name;
  const char*             display_name;
  const char*             icon_name;
  u32                     bind_index;
  input_binding_type_t    type;
  generic_input_binding_t generic_mapping;
} controller_binding_info_t;

 /* Top-level info for a controller variant.  `bindings_count` /
 * `settings_count` replace the C++ std::span pair. */
typedef struct {
  controller_type_t                type;
  const char*                      name;
  const char*                      display_name;
  const char*                      icon_name;
  const controller_binding_info_t* bindings;
  size_t                           bindings_count;
  const setting_info_t*            settings;
  size_t                           settings_count;
} controller_info_t;

#define CONTROLLER_DEFAULT_STICK_DEADZONE     0.0f
#define CONTROLLER_DEFAULT_STICK_SENSITIVITY  1.33f
#define CONTROLLER_DEFAULT_BUTTON_DEADZONE    0.25f

struct controller_vtable {
  /* Owning destructor: frees subclass-owned resources, then free(self). */
  void              (*destroy)(controller_t* self);
  controller_type_t (*get_type)(const controller_t* self);

  void  (*reset)(controller_t* self);
  bool  (*do_state)(controller_t* self, state_wrapper_t* sw, bool apply_input_state);

  void  (*reset_transfer_state)(controller_t* self);
  /* Returns ACK; *data_out is filled with the response byte. */
  bool  (*transfer)(controller_t* self, u8 data_in, u8* data_out);

  float (*get_bind_state)(const controller_t* self, u32 index);
  void  (*set_bind_state)(controller_t* self, u32 index, float value);
  u32   (*get_button_state_bits)(const controller_t* self);
  /* Returns false if the controller has no analog stick state to report. */
  bool  (*get_analog_input_bytes)(const controller_t* self, u32* out_bytes);

  void  (*load_settings)(controller_t* self, const settings_interface_t* si,
                         const char* section, bool initial);
};

struct controller {
  const controller_vtable_t* vtbl;
  u32                        index;
};

ALWAYS_INLINE controller_type_t controller_get_type(const controller_t* c)
{
  return c->vtbl->get_type(c);
}

ALWAYS_INLINE u32 controller_get_index(const controller_t* c) { return c->index; }

void  controller_destroy             (controller_t* c);
void  controller_reset               (controller_t* c);
bool  controller_do_state            (controller_t* c, state_wrapper_t* sw, bool apply_input_state);
void  controller_reset_transfer_state(controller_t* c);
bool  controller_transfer            (controller_t* c, u8 data_in, u8* data_out);
float controller_get_bind_state      (const controller_t* c, u32 index);
void  controller_set_bind_state      (controller_t* c, u32 index, float value);
u32   controller_get_button_state_bits(const controller_t* c);
bool  controller_get_analog_input_bytes(const controller_t* c, u32* out_bytes);
void  controller_load_settings       (controller_t* c, const settings_interface_t* si,
                                      const char* section, bool initial);

/* Allocates and returns a fresh controller of the requested type, or NULL for
 * CONTROLLER_TYPE_NONE / any unsupported variant.  Caller takes ownership and
 * must release via controller_destroy(). */
controller_t* controller_create(controller_type_t type, u32 index);

/* Static info table lookups. */
const controller_info_t* controller_get_info_for_type(controller_type_t type);
const controller_info_t* controller_get_info_for_name(const char* name);

/* Returns ARRAY of NUM controller_type_t pointers indexed by enum value.
 * Slots for unported variants are NULL. */
const controller_info_t* const* controller_get_info_list(size_t* out_count);

/* Helpers (lifted verbatim from C++). */
bool controller_in_circular_deadzone(float deadzone, float pos_x, float pos_y);

void controller_convert_pad_to_port_and_slot(u32 index, u32* out_port, u32* out_slot);
u32  controller_convert_port_and_slot_to_pad(u32 port, u32 slot);

bool controller_pad_is_multitap_slot      (u32 index);
bool controller_port_and_slot_is_multitap (u32 port, u32 slot);

/* Writes a "PadN" section name into `out` (must hold at least 8 bytes). */
void        controller_get_settings_section(u32 pad, char* out, size_t out_len);
const char* controller_get_port_display_name_pos(u32 port, u32 slot, bool mtap);
const char* controller_get_port_display_name    (u32 index);

/* True when the game database & user settings allow auto-analog. */
bool controller_can_start_in_analog_mode(controller_type_t type);

/* Display order used by the frontend (8 entries). */
extern const u32 g_controller_port_display_order[NUM_CONTROLLER_AND_CARD_PORTS];

#endif /* CUPID_CORE_CONTROLLER_H */
