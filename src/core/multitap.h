/*
 * 4-port multitap multiplexer.  When SetEnable(true, base_index) is called
 * the multitap takes over the SIO0 byte stream for its range, demultiplexing
 * controller / memory-card transfers based on the access byte (0x01..0x04
 * for pads, 0x81..0x84 for cards).  Reads through the multitap are routed
 * back through the global pad slot array via pad_get_controller /
 * pad_get_memory_card; the multitap itself owns no devices.
 */

#ifndef CUPID_CORE_MULTITAP_H
#define CUPID_CORE_MULTITAP_H

#include "core/controller.h"

#include "common/types.h"

typedef struct state_wrapper state_wrapper_t;

typedef enum : u8 {
  MULTITAP_TRANSFER_IDLE,
  MULTITAP_TRANSFER_MEMORY_CARD,
  MULTITAP_TRANSFER_CONTROLLER_COMMAND,
  MULTITAP_TRANSFER_SINGLE_CONTROLLER,
  MULTITAP_TRANSFER_ALL_CONTROLLERS,
} multitap_transfer_state_t;

typedef struct multitap {
  multitap_transfer_state_t transfer_state;
  u8                        selected_slot;

  u32  controller_transfer_step;

  bool invalid_transfer_all_command;
  bool transfer_all_controllers;
  bool current_controller_done;

  u8   transfer_buffer[32];

  u32  base_index;
  bool enabled;
} multitap_t;

void multitap_init (multitap_t* m);
void multitap_reset(multitap_t* m);

/* Plug/unplug the multitap.  When enabled, the controller / memcard slots
 * referenced via base_index, +1, +2, +3 become its multiplexed children. */
void multitap_set_enable(multitap_t* m, bool enable, u32 base_index);

ALWAYS_INLINE bool multitap_is_enabled(const multitap_t* m) { return m->enabled; }
ALWAYS_INLINE bool multitap_is_reading_memory_card(const multitap_t* m)
{
  return m->enabled && m->transfer_state == MULTITAP_TRANSFER_MEMORY_CARD;
}

bool multitap_do_state(multitap_t* m, state_wrapper_t* sw);

void multitap_reset_transfer_state(multitap_t* m);
bool multitap_transfer(multitap_t* m, u8 data_in, u8* data_out);

#endif /* CUPID_CORE_MULTITAP_H */
