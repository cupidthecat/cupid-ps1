/*
 * SIO0 device implementing the standard PS1 1Mbit memory card protocol.
 * Each Transfer() call advances a one-byte state machine; reads return
 * fixed handshake bytes interleaved with sector data, writes echo the
 * previous byte while the card stages 128 bytes into its frame buffer,
 * and the GET_ID command returns a static {04 00 00 80} block.
 *
 * Externs (see core/system.* once ported):
 *   system_get_ticks_per_second ; for the lazy save-back delay
 *   system_on_memory_card_accessed; OSD "card touched" hook
 * Both are declared __attribute__((weak)) so this TU compiles before either
 * lands; the weak fallback returns the PS1 master clock / does nothing.
 */

#include "core/memory_card.h"

#include "core/memory_card_image.h"
#include "core/timing_event.h"
#include "core/types.h"

#include "util/state_wrapper.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(MemoryCard);

/* PS1 master/CPU clock. */
#define MEMORY_CARD_MASTER_CLOCK 33868800

/* Save in five seconds, long enough for everything mid-write to finish. */
enum { MEMORY_CARD_SAVE_DELAY_IN_SECONDS = 5 };

extern tick_count_t system_get_ticks_per_second(void) __attribute__((weak));
extern void         system_on_memory_card_accessed(void) __attribute__((weak));

static tick_count_t memory_card_get_save_delay_in_ticks(void)
{
  const tick_count_t tps = system_get_ticks_per_second
                             ? system_get_ticks_per_second()
                             : (tick_count_t)MEMORY_CARD_MASTER_CLOCK;
  return tps * (tick_count_t)MEMORY_CARD_SAVE_DELAY_IN_SECONDS;
}

static void memory_card_on_accessed(void)
{
  if (system_on_memory_card_accessed)
    system_on_memory_card_accessed();
}

static char* mc_strdup(const char* s)
{
  if (!s) return NULL;
  const size_t len = strlen(s);
  char* d = (char*)malloc(len + 1u);
  if (!d) return NULL;
  memcpy(d, s, len + 1u);
  return d;
}

static bool memory_card_save_if_changed(memory_card_t* mc, bool display_osd_message);
static void memory_card_queue_file_save(memory_card_t* mc);
static void memory_card_save_event_cb (void* user, tick_count_t ticks, tick_count_t ticks_late);

static void memory_card_init(memory_card_t* mc, u32 index)
{
  memset(mc, 0, sizeof(*mc));
  mc->index = index;
  mc->state = MEMORY_CARD_STATE_IDLE;
  /* On a virgin card the BIOS expects the "no write yet" flag to be set
   * until the first WRITE command completes. */
  mc->flag = MEMORY_CARD_FLAG_NO_WRITE_YET;

  /* Build the per-card event name once.  The timing_event_t holds a
   * borrowed (data, len) view, so the storage must live as long as the
   * event; keeping it inside the card guarantees that. */
  const int n = snprintf(mc->save_event_name, sizeof(mc->save_event_name),
                         "Memory Card %u Host Flush", (unsigned)(index + 1u));
  const u32 name_len = (n > 0 && (size_t)n < sizeof(mc->save_event_name))
                         ? (u32)n
                         : (u32)(sizeof(mc->save_event_name) - 1u);

  const tick_count_t delay = memory_card_get_save_delay_in_ticks();
  timing_event_init(&mc->save_event,
                     mc->save_event_name, name_len,
                    delay, delay, 
                    &memory_card_save_event_cb, mc);
}

memory_card_t* memory_card_create(u32 index)
{
  memory_card_t* mc = (memory_card_t*)malloc(sizeof(memory_card_t));
  if (!mc) return NULL;
  memory_card_init(mc, index);
  memory_card_format(mc);
  return mc;
}

memory_card_t* memory_card_open(u32 index, const char* path)
{
  memory_card_t* mc = (memory_card_t*)malloc(sizeof(memory_card_t));
  if (!mc) return NULL;
  memory_card_init(mc, index);
  mc->path = mc_strdup(path);

  Error err = ERROR_INIT;
  if (!fs_file_exists(memory_card_get_path(mc)))
  {
    /* No backing file yet; start blank.  Don't queue a save: the BIOS
     * will reformat anyway, and we want the file to materialise only after
     * a real write. */
    memory_card_format(mc);
    mc->changed = false;
  }
  else if (!memory_card_image_load_from_file(mc->data, memory_card_get_path(mc), &err))
  {
    const char* fname_d;
    u32 fname_l;
    path_get_file_name_cstr(memory_card_get_path(mc), &fname_d, &fname_l);
    ERROR_LOG("Memory card %u could not be read: file=%.*s err=%s",
              (unsigned)(index + 1u), (int)fname_l, fname_d, Error_get_description(&err));
    /* Fall back to a freshly-formatted in-memory pad with no save-back path
     * so we don't clobber the broken file. */
    memory_card_format(mc);
    free(mc->path);
    mc->path = NULL;
    mc->changed = false;
  }
  Error_destroy(&err);

  return mc;
}

void memory_card_destroy(memory_card_t* mc)
{
  if (!mc) return;
  memory_card_save_if_changed(mc, false);
  if (timing_event_is_active(&mc->save_event))
    timing_event_deactivate(&mc->save_event);
  timing_event_destroy(&mc->save_event);
  free(mc->path);
  free(mc);
}

void memory_card_reset(memory_card_t* mc)
{
  memory_card_reset_transfer_state(mc);
  memory_card_save_if_changed(mc, true);
  mc->flag = MEMORY_CARD_FLAG_NO_WRITE_YET;
}

bool memory_card_do_state(memory_card_t* mc, state_wrapper_t* sw)
{
  /* On load we may be replacing a card mid-flight; flush first so we don't
   * silently lose pending writes from the outgoing instance. */
  if (state_wrapper_is_reading(sw))
    memory_card_save_if_changed(mc, true);

  u8 state_u8 = (u8)mc->state;
  state_wrapper_do_u8(sw, &state_u8);
  mc->state = (memory_card_state_t)state_u8;

  state_wrapper_do_u8(sw, &mc->flag);
  state_wrapper_do_u16(sw, &mc->address);
  state_wrapper_do_u8(sw, &mc->sector_offset);
  state_wrapper_do_u8(sw, &mc->checksum);
  state_wrapper_do_u8(sw, &mc->last_byte);
  state_wrapper_do_bytes(sw, mc->data, sizeof(mc->data));
  state_wrapper_do_bool(sw, &mc->changed);

  return !state_wrapper_has_error(sw);
}

void memory_card_copy_state(memory_card_t* dst, const memory_card_t* src)
{
  /* Used for runahead/rewind state copies; the data array is assumed to */
  DebugAssert(memcmp(dst->data, src->data, sizeof(dst->data)) == 0);

  dst->state         = src->state;
  dst->flag          = src->flag;
  dst->address       = src->address;
  dst->sector_offset = src->sector_offset;
  dst->checksum      = src->checksum;
  dst->last_byte     = src->last_byte;
  dst->changed       = src->changed;
}

void memory_card_reset_transfer_state(memory_card_t* mc)
{
  mc->state         = MEMORY_CARD_STATE_IDLE;
  mc->address       = 0;
  mc->sector_offset = 0;
  mc->checksum      = 0;
  mc->last_byte     = 0;
}

bool memory_card_transfer(memory_card_t* mc, u8 data_in, u8* data_out)
{
  bool ack = false;
#if !defined(NDEBUG)
  const memory_card_state_t old_state = mc->state;
#endif

  switch (mc->state)
  {
    case MEMORY_CARD_STATE_READ_CARD_ID1:
      *data_out = 0x5A; ack = true; mc->state = MEMORY_CARD_STATE_READ_CARD_ID2; break;
    case MEMORY_CARD_STATE_READ_CARD_ID2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_READ_ADDRESS_MSB; break;

    case MEMORY_CARD_STATE_READ_ADDRESS_MSB:
      *data_out = 0x00;
      ack = true;
      mc->address = (u16)(((mc->address & 0x00FFu) | (ZeroExtend16(data_in) << 8)) & 0x3FFu);
      mc->state = MEMORY_CARD_STATE_READ_ADDRESS_LSB;
      break;

    case MEMORY_CARD_STATE_READ_ADDRESS_LSB:
      *data_out = mc->last_byte;
      ack = true;
      mc->address = (u16)(((mc->address & 0xFF00u) | ZeroExtend16(data_in)) & 0x3FFu);
      mc->sector_offset = 0;
      mc->state = MEMORY_CARD_STATE_READ_ACK1;
      break;

    case MEMORY_CARD_STATE_READ_ACK1:
      *data_out = 0x5C; ack = true; mc->state = MEMORY_CARD_STATE_READ_ACK2; break;
    case MEMORY_CARD_STATE_READ_ACK2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_MSB; break;

    case MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_MSB:
      *data_out = Truncate8(mc->address >> 8); ack = true;
      mc->state = MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_LSB;
      break;
    case MEMORY_CARD_STATE_READ_CONFIRM_ADDRESS_LSB:
      *data_out = Truncate8(mc->address); ack = true;
      mc->state = MEMORY_CARD_STATE_READ_DATA;
      break;

    case MEMORY_CARD_STATE_READ_DATA:
    {
      const u8 bits = mc->data[ZeroExtend32(mc->address) * MEMORY_CARD_IMAGE_FRAME_SIZE + mc->sector_offset];
      if (mc->sector_offset == 0)
      {
        DEV_LOG("Reading memory card sector %u", (unsigned)mc->address);
        /* Checksum is XOR of high addr byte ^ low addr byte ^ all 128 data bytes. */
        mc->checksum = (u8)(Truncate8(mc->address >> 8) ^ Truncate8(mc->address) ^ bits);
        memory_card_on_accessed();
      }
      else
      {
        mc->checksum ^= bits;
      }

      *data_out = bits;
      ack = true;

      mc->sector_offset++;
      if (mc->sector_offset == MEMORY_CARD_IMAGE_FRAME_SIZE)
      {
        mc->state = MEMORY_CARD_STATE_READ_CHECKSUM;
        mc->sector_offset = 0;
      }
      break;
    }

    case MEMORY_CARD_STATE_READ_CHECKSUM:
      *data_out = mc->checksum; ack = true; mc->state = MEMORY_CARD_STATE_READ_END; break;
    case MEMORY_CARD_STATE_READ_END:
      *data_out = 0x47; ack = true; mc->state = MEMORY_CARD_STATE_IDLE; break;

    case MEMORY_CARD_STATE_WRITE_CARD_ID1:
      *data_out = 0x5A; ack = true; mc->state = MEMORY_CARD_STATE_WRITE_CARD_ID2; break;
    case MEMORY_CARD_STATE_WRITE_CARD_ID2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_WRITE_ADDRESS_MSB; break;

    case MEMORY_CARD_STATE_WRITE_ADDRESS_MSB:
      *data_out = 0x00;
      ack = true;
      mc->address = (u16)(((mc->address & 0x00FFu) | (ZeroExtend16(data_in) << 8)) & 0x3FFu);
      mc->state = MEMORY_CARD_STATE_WRITE_ADDRESS_LSB;
      break;

    case MEMORY_CARD_STATE_WRITE_ADDRESS_LSB:
      *data_out = mc->last_byte;
      ack = true;
      mc->address = (u16)(((mc->address & 0xFF00u) | ZeroExtend16(data_in)) & 0x3FFu);
      mc->sector_offset = 0;
      mc->state = MEMORY_CARD_STATE_WRITE_DATA;
      break;

    case MEMORY_CARD_STATE_WRITE_DATA:
    {
      if (mc->sector_offset == 0)
      {
        INFO_LOG("Writing memory card sector %u", (unsigned)mc->address);
        mc->checksum = (u8)(Truncate8(mc->address >> 8) ^ Truncate8(mc->address) ^ data_in);
        /* Clear the "no write yet" flag the moment the host actually writes. */
        mc->flag &= (u8)~MEMORY_CARD_FLAG_NO_WRITE_YET;
        memory_card_on_accessed();
      }
      else
      {
        mc->checksum ^= data_in;
      }

      const u32 offset = ZeroExtend32(mc->address) * MEMORY_CARD_IMAGE_FRAME_SIZE + mc->sector_offset;
      mc->changed = mc->changed || (mc->data[offset] != data_in);
      mc->data[offset] = data_in;

      *data_out = mc->last_byte;
      ack = true;

      mc->sector_offset++;
      if (mc->sector_offset == MEMORY_CARD_IMAGE_FRAME_SIZE)
      {
        mc->state = MEMORY_CARD_STATE_WRITE_CHECKSUM;
        mc->sector_offset = 0;
        if (mc->changed)
          memory_card_queue_file_save(mc);
      }
      break;
    }

    case MEMORY_CARD_STATE_WRITE_CHECKSUM:
      *data_out = mc->checksum; ack = true; mc->state = MEMORY_CARD_STATE_WRITE_ACK1; break;
    case MEMORY_CARD_STATE_WRITE_ACK1:
      *data_out = 0x5C; ack = true; mc->state = MEMORY_CARD_STATE_WRITE_ACK2; break;
    case MEMORY_CARD_STATE_WRITE_ACK2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_WRITE_END; break;
    case MEMORY_CARD_STATE_WRITE_END:
      /* WRITE_END returns 0x47 *without* asserting /ACK; the host
       * deselects the card here, so any further byte starts a new command. */
      *data_out = 0x47; ack = false; mc->state = MEMORY_CARD_STATE_IDLE; break;

    case MEMORY_CARD_STATE_GET_ID_CARD_ID1:
      *data_out = 0x5A; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID_CARD_ID2; break;
    case MEMORY_CARD_STATE_GET_ID_CARD_ID2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID_ACK1; break;
    case MEMORY_CARD_STATE_GET_ID_ACK1:
      *data_out = 0x5C; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID_ACK2; break;
    case MEMORY_CARD_STATE_GET_ID_ACK2:
      *data_out = 0x5D; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID1; break;
    case MEMORY_CARD_STATE_GET_ID1:
      *data_out = 0x04; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID2; break;
    case MEMORY_CARD_STATE_GET_ID2:
      *data_out = 0x00; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID3; break;
    case MEMORY_CARD_STATE_GET_ID3:
      *data_out = 0x00; ack = true; mc->state = MEMORY_CARD_STATE_GET_ID4; break;
    case MEMORY_CARD_STATE_GET_ID4:
      *data_out = 0x80; ack = true; mc->state = MEMORY_CARD_STATE_COMMAND; break;

    case MEMORY_CARD_STATE_IDLE:
      /* SIO0 device-select byte for memory cards is 0x81. */
      if (data_in == 0x81)
      {
        *data_out = 0xFF;
        ack = true;
        mc->state = MEMORY_CARD_STATE_COMMAND;
      }
      break;

    case MEMORY_CARD_STATE_COMMAND:
      switch (data_in)
      {
        case 0x52: /* read data */
          *data_out = mc->flag;
          ack = true;
          mc->state = MEMORY_CARD_STATE_READ_CARD_ID1;
          break;

        case 0x57: /* write data */
          *data_out = mc->flag;
          ack = true;
          mc->state = MEMORY_CARD_STATE_WRITE_CARD_ID1;
          break;

        case 0x53: /* get id */
          *data_out = mc->flag;
          ack = true;
          mc->state = MEMORY_CARD_STATE_GET_ID_CARD_ID1;
          break;

        default:
          /* Unknown command; echo FLAG, drop /ACK to abort the transfer. */
          ERROR_LOG("Invalid command 0x%02X", (unsigned)data_in);
          *data_out = mc->flag;
          ack = false;
          mc->state = MEMORY_CARD_STATE_IDLE;
          break;
      }
      break;

    default:
      UnreachableCode();
      break;
  }

#if !defined(NDEBUG)
  DEBUG_LOG("Transfer, old_state=%u, new_state=%u, data_in=0x%02X, data_out=0x%02X, ack=%s",
            (unsigned)old_state, (unsigned)mc->state, (unsigned)data_in, (unsigned)*data_out,
            ack ? "true" : "false");
#endif
  mc->last_byte = data_in;
  return ack;
}

bool memory_card_is_or_was_recently_writing(const memory_card_t* mc)
{
  /* Either currently shifting a sector in, or the deferred flush hasn't
   * fired yet; both count as "actively saving" for OSD purposes. */
  return (mc->state == MEMORY_CARD_STATE_WRITE_DATA) || timing_event_is_active(&mc->save_event);
}

void memory_card_format(memory_card_t* mc)
{
  memory_card_image_format(mc->data);
  mc->changed = true;
}

static bool memory_card_save_if_changed(memory_card_t* mc, bool display_osd_message)
{
  (void)display_osd_message; /* OSD layer not ported yet. */

  if (timing_event_is_active(&mc->save_event))
    timing_event_deactivate(&mc->save_event);

  if (!mc->changed)
    return true;

  mc->changed = false;

  if (!mc->path || mc->path[0] == '\0')
    return false;

  const char* fname_d;
  u32 fname_l;
  path_get_file_name_cstr(mc->path, &fname_d, &fname_l);
  INFO_LOG("Saving memory card to %.*s...", (int)fname_l, fname_d);

  Error err = ERROR_INIT;
  if (!memory_card_image_save_to_file(mc->data, mc->path, &err))
  {
    ERROR_LOG("Failed to save memory card %u: %s",
              (unsigned)(mc->index + 1u), Error_get_description(&err));
    Error_destroy(&err);
    return false;
  }
  Error_destroy(&err);

  return true;
}

static void memory_card_queue_file_save(memory_card_t* mc)
{
  /* Skip if a save is already pending or there's nowhere to write. */
  if (timing_event_is_active(&mc->save_event) || !mc->path || mc->path[0] == '\0')
    return;

  timing_event_schedule(&mc->save_event, memory_card_get_save_delay_in_ticks());
}

static void memory_card_save_event_cb(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)ticks;
  (void)ticks_late;
  memory_card_save_if_changed((memory_card_t*)user, true);
}
