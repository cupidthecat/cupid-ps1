#include "core/controller.h"
#include "core/analog_controller.h"
#include "core/analog_joystick.h"
#include "core/ddgo_controller.h"
#include "core/digital_controller.h"
#include "core/game_database.h"
#include "core/guncon.h"
#include "core/jogcon.h"
#include "core/justifier.h"
#include "core/negcon.h"
#include "core/playstation_mouse.h"
#include "core/settings.h"
#include "core/system.h"

#include "common/assert.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Local stand-in for the "None" entry: no bindings, no settings. */
static const controller_info_t s_none_info = {
   .type           = CONTROLLER_TYPE_NONE,
  .name           = "None",
  .display_name   = "Not Connected",
  .icon_name      = NULL,
  .bindings       = NULL,
  .bindings_count = 0,
  .settings       = NULL,
  .settings_count = 0, 
};

/* The full table includes deferred variants (analog joystick, mouse, etc.)
 * as NULL slots; present so save-state mismatch warnings still produce a
 * legible "Unknown" label, and the array length stays in sync with the enum. */
static const controller_info_t* const s_controller_info[CONTROLLER_TYPE_COUNT] = {
  [CONTROLLER_TYPE_NONE]               = &s_none_info,
  [CONTROLLER_TYPE_DIGITAL_CONTROLLER] = &g_digital_controller_info,
  [CONTROLLER_TYPE_ANALOG_CONTROLLER]  = &g_analog_controller_info,
  [CONTROLLER_TYPE_ANALOG_JOYSTICK]    = &g_analog_joystick_info,
  [CONTROLLER_TYPE_GUNCON]             = &g_guncon_info,
  [CONTROLLER_TYPE_JUSTIFIER]          = &g_justifier_info,
  [CONTROLLER_TYPE_PLAYSTATION_MOUSE]  = &g_playstation_mouse_info,
  [CONTROLLER_TYPE_NEGCON]             = &g_negcon_info,
  [CONTROLLER_TYPE_DDGO_CONTROLLER]    = &g_ddgo_controller_info,
  [CONTROLLER_TYPE_JOGCON]             = &g_jogcon_info, 
  /* Other slots default-initialised to NULL. */
};

const u32 g_controller_port_display_order[NUM_CONTROLLER_AND_CARD_PORTS] = {0, 2, 3, 4, 1, 5, 6, 7};

void controller_destroy(controller_t* c)
{
  if (!c) return;
  c->vtbl->destroy(c);
}

void controller_reset(controller_t* c)
{
  if (c->vtbl->reset) c->vtbl->reset(c);
}

bool controller_do_state(controller_t* c, state_wrapper_t* sw, bool apply_input_state)
{
  if (c->vtbl->do_state) return c->vtbl->do_state(c, sw, apply_input_state);
  return true;
}

void controller_reset_transfer_state(controller_t* c)
{
  if (c->vtbl->reset_transfer_state) c->vtbl->reset_transfer_state(c);
}

bool controller_transfer(controller_t* c, u8 data_in, u8* data_out)
{
  if (c->vtbl->transfer) return c->vtbl->transfer(c, data_in, data_out);
  *data_out = 0xFF;
  return false;
}

float controller_get_bind_state(const controller_t* c, u32 index)
{
  if (c->vtbl->get_bind_state) return c->vtbl->get_bind_state(c, index);
  return 0.0f;
}

void controller_set_bind_state(controller_t* c, u32 index, float value)
{
  if (c->vtbl->set_bind_state) c->vtbl->set_bind_state(c, index, value);
}

u32 controller_get_button_state_bits(const controller_t* c)
{
  if (c->vtbl->get_button_state_bits) return c->vtbl->get_button_state_bits(c);
  return 0;
}

bool controller_get_analog_input_bytes(const controller_t* c, u32* out_bytes)
{
  if (c->vtbl->get_analog_input_bytes) return c->vtbl->get_analog_input_bytes(c, out_bytes);
  return false;
}

void controller_load_settings(controller_t* c, const settings_interface_t* si,
                              const char* section, bool initial)
{
  if (c->vtbl->load_settings) c->vtbl->load_settings(c, si, section, initial);
}

controller_t* controller_create(controller_type_t type, u32 index)
{
  switch (type)
  {
    case CONTROLLER_TYPE_DIGITAL_CONTROLLER:
    case CONTROLLER_TYPE_POPN_CONTROLLER:
      return digital_controller_create(index, type);

    case CONTROLLER_TYPE_ANALOG_CONTROLLER:
      return analog_controller_create(index);

    case CONTROLLER_TYPE_ANALOG_JOYSTICK:
      return analog_joystick_create(index);

    case CONTROLLER_TYPE_PLAYSTATION_MOUSE:
      return playstation_mouse_create(index);

    case CONTROLLER_TYPE_NEGCON:
      return negcon_create(index);

    case CONTROLLER_TYPE_GUNCON:
      return guncon_create(index);

    case CONTROLLER_TYPE_JUSTIFIER:
      return justifier_create(index);

    case CONTROLLER_TYPE_JOGCON:
      return jogcon_create(index);

    case CONTROLLER_TYPE_DDGO_CONTROLLER:
      return ddgo_controller_create(index);

    /* Deferred variants: caller gets NULL and pad treats slot as empty. */
    case CONTROLLER_TYPE_NEGCON_RUMBLE:
    case CONTROLLER_TYPE_NONE:
    default:
      return NULL;
  }
}

const controller_info_t* controller_get_info_for_type(controller_type_t type)
{
  if ((u32)type >= (u32)CONTROLLER_TYPE_COUNT)
    return &s_none_info;
  const controller_info_t* info = s_controller_info[(u32)type];
  return info ? info : &s_none_info;
}

const controller_info_t* controller_get_info_for_name(const char* name)
{
  for (u32 i = 0; i < (u32)CONTROLLER_TYPE_COUNT; i++)
  {
    const controller_info_t* info = s_controller_info[i];
    if (info && strcmp(name, info->name) == 0)
      return info;
  }
  return NULL;
}

const controller_info_t* const* controller_get_info_list(size_t* out_count)
{
  if (out_count) *out_count = (size_t)CONTROLLER_TYPE_COUNT;
  return s_controller_info;
}

void controller_convert_pad_to_port_and_slot(u32 index, u32* out_port, u32* out_slot)
{
  if (index > 4)
  {
    /* [5,6,7] -> 2B,2C,2D */
    *out_port = 1;
    *out_slot = index - 4;
  }
  else if (index > 1)
  {
    /* [2,3,4] -> 1B,1C,1D */
    *out_port = 0;
    *out_slot = index - 1;
  }
  else
  {
    /* [0,1] -> 1A,2A */
    *out_port = index;
    *out_slot = 0;
  }
}

u32 controller_convert_port_and_slot_to_pad(u32 port, u32 slot)
{
  if (slot == 0)
    return port;
  if (port == 0) /* slot in [1,3] -> 2,3,4 */
    return slot + 1;
  return slot + 4; /* port==1, slot in [1,3] -> 5,6,7 */
}

bool controller_pad_is_multitap_slot(u32 index) { return (index >= 2); }
bool controller_port_and_slot_is_multitap(u32 port, u32 slot) { (void)port; return (slot != 0); }

void controller_get_settings_section(u32 pad, char* out, size_t out_len)
{
  snprintf(out, out_len, "Pad%u", pad + 1u);
}

const char* controller_get_port_display_name_pos(u32 port, u32 slot, bool mtap)
{
  /* No-multitap labels for ports 1/2; multitap labels are <port><slot-letter>. */
  static const char* const no_mtap_labels[NUM_MULTITAPS] = {"1", "2"};
  static const char* const mtap_labels[NUM_MULTITAPS][NUM_CONTROLLER_AND_CARD_PORTS_PER_MULTITAP] = {
    {"1A", "1B", "1C", "1D"},
    {"2A", "2B", "2C", "2D"},
  };
  DebugAssert(port < NUM_MULTITAPS && slot < NUM_CONTROLLER_AND_CARD_PORTS_PER_MULTITAP);
  return mtap ? mtap_labels[port][slot] : no_mtap_labels[port];
}

/* The per-port multitap status would normally come from g_settings via the
 * settings module; the headless boot path doesn't enable multitaps so we
 * always return the simple "1"/"2" label.  When multitap config is added,
 * this routine should consult settings_is_multitap_port_enabled(). */
const char* controller_get_port_display_name(u32 index)
{
  u32 port, slot;
  controller_convert_pad_to_port_and_slot(index, &port, &slot);
  return controller_get_port_display_name_pos(port, slot, false);
}

bool controller_in_circular_deadzone(float deadzone, float pos_x, float pos_y)
{
  const float distance = sqrtf(pos_x * pos_x + pos_y * pos_y);
  return (distance <= deadzone);
}

bool controller_can_start_in_analog_mode(controller_type_t type)
{
  /* Only DualShock-style pads can ever auto-enter analog mode. */
  if (type != CONTROLLER_TYPE_ANALOG_CONTROLLER)
    return false;

  /* User opted out of GameDB compatibility overlay; trust default-on. */
  if (!g_settings.apply_compatibility_settings)
    return true;

  const game_database_entry_t* e = system_get_game_database_entry();
  if (!e)
    return true;  /* no GameDB info (BIOS/EXE/PSF/unknown disc) -> default on */

  /* If the entry has an explicit `controllers:` list (i.e. supported_controllers
   * is neither 0 = "no list" nor 0xFFFF = "all"), and that list does not
   * include AnalogController, the game cannot speak DualShock at all; start
   * in digital mode so we report ID 0x41.  Crash Bandicoot SCUS-94900 is the
   * canonical case (its YAML lists only DigitalController). */
  if (e->supported_controllers != 0 &&
      e->supported_controllers != (u16)0xFFFFu) {
    const u16 mask = (u16)(1u << (u32)CONTROLLER_TYPE_ANALOG_CONTROLLER);
    if ((e->supported_controllers & mask) == 0)
      return false;
  }

  /* Some games support AnalogController but reject auto-on (the trait is set
   * by hand in gamedb.yaml for known offenders). */
  return !game_database_entry_has_trait(
    e, GAME_DATABASE_TRAIT_DISABLE_AUTO_ANALOG_MODE);
}
