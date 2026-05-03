/*
 * MemoryCard is the SIO0-bus device that implements the standard PS1 1Mbit
 * memory card protocol.  In C++ this is a concrete class (no virtuals); we
 * lower it to a plain memory_card_t struct + memory_card_* flat funcs.
 *
 * The 128KB image lives inline in the struct (data[]); there is no shared
 * buffer.  The save event (timing_event_t) drives lazy write-back to the
 * backing path; m_changed is sticky until SaveIfChanged clears it.
 */

#ifndef CUPID_CORE_MEMORY_CARD_H
#define CUPID_CORE_MEMORY_CARD_H

#include "core/memory_card_image.h"
#include "core/timing_event.h"
#include "core/types.h"

typedef struct state_wrapper state_wrapper_t;
typedef struct Error         Error;

 /*
 * exactly so the FIXED_REPLY_STATE / ADDRESS_STATE_MSB tables stay readable
 * against the original.  Each card has its own copy. */
typedef enum {
  MEMORY_CARD_STATE_IDLE = 0,
  MEMORY_CARD_STATE_COMMAND,

  MEMORY_CARD_STATE_READ_CARD_ID1,
  MEMORY_CARD_STATE_READ_CARD_ID2,
  MEMORY_CARD_STATE_READ_ADDRESS_MSB,
  MEMORY_CARD_STATE_READ_ADDRESS_LSB,
  MEMORY_CARD_STATE_READ_ACK1,
  MEMORY_CARD_STATE_READ_ACK2,
  MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_MSB,
  MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_LSB,
  MEMORY_CARD_STATE_READ_DATA,
  MEMORY_CARD_STATE_READ_CHECKSUM,
  MEMORY_CARD_STATE_READ_END,

  MEMORY_CARD_STATE_WRITE_CARD_ID1,
  MEMORY_CARD_STATE_WRITE_CARD_ID2,
  MEMORY_CARD_STATE_WRITE_ADDRESS_MSB,
  MEMORY_CARD_STATE_WRITE_ADDRESS_LSB,
  MEMORY_CARD_STATE_WRITE_DATA,
  MEMORY_CARD_STATE_WRITE_CHECKSUM,
  MEMORY_CARD_STATE_WRITE_ACK1,
  MEMORY_CARD_STATE_WRITE_ACK2,
  MEMORY_CARD_STATE_WRITE_END,

  MEMORY_CARD_STATE_GET_ID_CARD_ID1,
  MEMORY_CARD_STATE_GET_ID_CARD_ID2,
  MEMORY_CARD_STATE_GET_ID_ACK1,
  MEMORY_CARD_STATE_GET_ID_ACK2,
  MEMORY_CARD_STATE_GET_ID1,
  MEMORY_CARD_STATE_GET_ID2,
  MEMORY_CARD_STATE_GET_ID3,
  MEMORY_CARD_STATE_GET_ID4,
} memory_card_state_t;

/* FLAG byte returned in the second byte of every command:
 *   bit 3 = no_write_yet  (cleared by first successful write)
 *   bit 2 = write_error   (set on write_error from the card) */
enum {
  MEMORY_CARD_FLAG_WRITE_ERROR  = (1u << 2),
  MEMORY_CARD_FLAG_NO_WRITE_YET = (1u << 3),
};

typedef struct memory_card {
  /* Save-event name lives in card storage so its (data,len) view stays
   * stable (timing_event_t holds a borrowed view, not a copy). */
  char save_event_name[32];

  timing_event_t save_event;

  /* Path to the backing file on disk.  Heap-allocated, NUL-terminated;
   * NULL/empty string means "no persistent storage" (e.g. non-persistent
   * memory cards). */
  char* path;

  u32                 index;
  memory_card_state_t state;
  u8                  flag;          /* FLAG byte; bit semantics above */
  u16                 address;       /* current sector during read/write */
  u8                  sector_offset; /* byte index within the 128B frame */
  u8                  checksum;      /* running XOR over (addr_hi, addr_lo, data...) */
  u8                  last_byte;     /* previous data_in; echoed in some replies */
  bool                changed;       /* dirty, needs flushing to disk */

  u8 data[MEMORY_CARD_IMAGE_DATA_SIZE];
} memory_card_t;

enum {
  MEMORY_CARD_STATE_SIZE = 1 + 1 + 2 + 1 + 1 + 1 + MEMORY_CARD_IMAGE_DATA_SIZE + 1,
};

memory_card_t* memory_card_create(u32 index);
memory_card_t* memory_card_open  (u32 index, const char* path);

/* Flushes any pending write back to disk and frees the card. */
void memory_card_destroy(memory_card_t* mc);

static inline u8*         memory_card_get_data      (memory_card_t* mc)       { return mc->data; }
static inline const u8*   memory_card_get_data_const(const memory_card_t* mc) { return mc->data; }
static inline const char* memory_card_get_path      (const memory_card_t* mc) { return mc->path ? mc->path : ""; }

void memory_card_reset(memory_card_t* mc);
bool memory_card_do_state(memory_card_t* mc, state_wrapper_t* sw);
void memory_card_copy_state(memory_card_t* dst, const memory_card_t* src);

/* Re-arm the SIO0 transfer state machine.  Called between commands. */
void memory_card_reset_transfer_state(memory_card_t* mc);

/* Process one byte of a SIO0 transfer.  `data_in` is what the host sent;
 * `*data_out` receives the card's reply.  Returns true if the card asserts
 * /ACK (i.e. wants the host to send another byte). */
bool memory_card_transfer(memory_card_t* mc, u8 data_in, u8* data_out);

/* True while the card is mid-write or has a pending lazy save scheduled --
 * used by the OSD to display the "saving" indicator. */
bool memory_card_is_or_was_recently_writing(const memory_card_t* mc);

/* Reformat the in-memory image and mark it dirty. */
void memory_card_format(memory_card_t* mc);

#endif /* CUPID_CORE_MEMORY_CARD_H */
