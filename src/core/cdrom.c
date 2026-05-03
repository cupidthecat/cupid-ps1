/*
 * PS1 CD-ROM controller emulation; the central state machine.  The mech
 * runs four timing events (command, command_second_response, async_interrupt,
 * drive) which together drive command dispatch and the sector pipeline.
 *
 * Hardware quirks worth keeping in mind:
 *   - INT3 (ACK) followed quickly by INT1 (DataReady) needs an enforced
 *     gap (MINIMUM_INTERRUPT_DELAY); games like Ogre Battle clear the IRQ
 *     before reading the response FIFO and depend on the INT1 *not* having
 *     overwritten the response window yet.
 *   - The sector-buffer ring (NUM_SECTOR_BUFFERS=8) lets the read pipeline
 *     stay 1-2 sectors ahead of the CPU; INT1 delivery flips the read
 *     pointer to the latest write pointer, mirroring the HC05 sub-CPU.
 *   - CD audio (XA-ADPCM and CDDA) bypasses the sector buffer and feeds an
 *     AUDIO_FIFO_SIZE samples-deep ring that the SPU pulls from.
 *   - Async reader handoff: the foreground side calls
 *     cdrom_async_reader_queue_read_sector() to start the next disc read,
 *     then cdrom_async_reader_wait_for_read_to_complete() to block on it
 *     before pulling the buffer/subq pointers.
 *
 *   - The ImGui debug window is omitted (boot path doesn't need it).
 *   - File-map (ISO directory walker) is omitted; it's a debug-only
 *     overlay.
 *   - Audio CD precaching message uses fputs to stderr (no OSD layer yet).
 *   - C++ std::deque<u8> FIFOs collapse to fixed-size circular byte buffers.
 *   - C++ HeapFIFOQueue<u32> for audio collapses to a heap-allocated u32
 *     ring of AUDIO_FIFO_SIZE entries.
 */

#include "core/cdrom.h"

#include "core/cpu_core.h"     /* g_cpu_state for param-write source PC trace */
#include "core/cpu_disasm.h"   /* cpu_disassemble for writer instruction */
#include "common/small_string.h"
#include "core/cdrom_async_reader.h"
#include "core/fmv_dump.h"
#include "core/cdrom_subq_replacement.h"
#include "core/dma.h"
#include "core/interrupt_controller.h"
#include "core/save_state_version.h"
#include "core/settings.h"
#include "core/timing_event.h"
#include "core/types.h"

#include "util/cd_image.h"
#include "util/state_wrapper.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/bcdutils.h"
#include "common/error.h"
#include "common/log.h"
#include "common/types.h"
#include "common/xorshift_prng.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDROM);

extern bool             mdec_is_active(void) __attribute__((weak));
extern void             spu_generate_pending_samples(void);
extern global_ticks_t   timing_events_get_global_tick_counter(void);
extern tick_count_t     system_scale_ticks_to_overclock(tick_count_t t) __attribute__((weak));
extern console_region_t system_get_region(void) __attribute__((weak));
extern console_region_t system_get_console_region_for_disc_region(disc_region_t r) __attribute__((weak));

/* Default fallbacks when system module isn't linked. */
static tick_count_t cdrom_scale_ticks_to_overclock(tick_count_t t)
{
  if (system_scale_ticks_to_overclock) return system_scale_ticks_to_overclock(t);
  if (!g_settings.cpu_overclock_active) return t;
  return (tick_count_t)(((u64)(u32)t * g_settings.cpu_overclock_numerator +
                         (g_settings.cpu_overclock_denominator - 1)) /
                        g_settings.cpu_overclock_denominator);
}

static console_region_t cdrom_get_console_region(void)
{
  return system_get_region ? system_get_region() : CONSOLE_REGION_NTSC_U;
}

static console_region_t cdrom_console_region_for_disc(disc_region_t r)
{
  if (system_get_console_region_for_disc_region)
    return system_get_console_region_for_disc_region(r);
  switch (r) {
    case DISC_REGION_NTSC_J: return CONSOLE_REGION_NTSC_J;
    case DISC_REGION_PAL:    return CONSOLE_REGION_PAL;
    default:                 return CONSOLE_REGION_NTSC_U;
  }
}

enum {
  CDROM_RAW_SECTOR_OUTPUT_SIZE        = CD_IMAGE_RAW_SECTOR_SIZE - CD_IMAGE_SECTOR_SYNC_SIZE, /* 2340 */
  CDROM_DATA_SECTOR_OUTPUT_SIZE       = CD_IMAGE_DATA_SECTOR_SIZE,                            /* 2048 */
  CDROM_SECTOR_SYNC_SIZE              = CD_IMAGE_SECTOR_SYNC_SIZE,
  CDROM_SECTOR_HEADER_SIZE            = CD_IMAGE_SECTOR_HEADER_SIZE,
  CDROM_MODE1_HEADER_SIZE             = CD_IMAGE_MODE1_HEADER_SIZE,
  CDROM_MODE2_HEADER_SIZE             = CD_IMAGE_MODE2_HEADER_SIZE,
  CDROM_SUBQ_SECTOR_SKEW              = 2,
  CDROM_XA_ADPCM_SAMPLES_PER_SECTOR_4BIT = 4032,
  CDROM_XA_ADPCM_SAMPLES_PER_SECTOR_8BIT = 2016,
  CDROM_XA_RESAMPLE_RING_BUFFER_SIZE  = 32,
  CDROM_PRNG_SEED                     = 0x4B435544u,

  CDROM_PARAM_FIFO_SIZE      = 16,
  CDROM_RESPONSE_FIFO_SIZE   = 16,
  CDROM_NUM_SECTOR_BUFFERS   = 8,
  CDROM_AUDIO_FIFO_SIZE      = 44100 * 2,
  CDROM_AUDIO_FIFO_LOW_WATERMARK = 10,

  CDROM_INIT_TICKS               = 4000000,
  CDROM_ID_READ_TICKS            = 33868,
  CDROM_MOTOR_ON_RESPONSE_TICKS  = 400000,

  CDROM_MAX_FAST_FORWARD_RATE    = 12,
  CDROM_FAST_FORWARD_RATE_STEP   = 4,

  CDROM_CDDA_REPORT_START_DELAY  = 60,
  CDROM_MINIMUM_INTERRUPT_DELAY  = 1000,
  CDROM_INTERRUPT_DELAY_CYCLES   = 500,
  CDROM_MISSED_INT1_DELAY_CYCLES = 5000,

  CDROM_SINGLE_SPEED_SECTORS_PER_SECOND = 75,
  CDROM_DOUBLE_SPEED_SECTORS_PER_SECOND = 150,

  CDROM_MASTER_CLOCK = 33868800, /* mirrors System::MASTER_CLOCK */
};

#define CDROM_INTERRUPT_REGISTER_MASK ((u8)0x1F)
#define CDROM_MIN_SEEK_TICKS          ((tick_count_t)30000)

typedef enum : u8 {
  CDROM_INT_DATA_READY = 0x01,
  CDROM_INT_COMPLETE   = 0x02,
  CDROM_INT_ACK        = 0x03,
  CDROM_INT_DATA_END   = 0x04,
  CDROM_INT_ERROR      = 0x05,
} cdrom_interrupt_t;

typedef enum : u16 {
  CDROM_CMD_SYNC      = 0x00,
  CDROM_CMD_GETSTAT   = 0x01,
  CDROM_CMD_SETLOC    = 0x02,
  CDROM_CMD_PLAY      = 0x03,
  CDROM_CMD_FORWARD   = 0x04,
  CDROM_CMD_BACKWARD  = 0x05,
  CDROM_CMD_READN     = 0x06,
  CDROM_CMD_MOTORON   = 0x07,
  CDROM_CMD_STOP      = 0x08,
  CDROM_CMD_PAUSE     = 0x09,
  CDROM_CMD_INIT      = 0x0A,
  CDROM_CMD_MUTE      = 0x0B,
  CDROM_CMD_DEMUTE    = 0x0C,
  CDROM_CMD_SETFILTER = 0x0D,
  CDROM_CMD_SETMODE   = 0x0E,
  CDROM_CMD_GETMODE   = 0x0F,
  CDROM_CMD_GETLOCL   = 0x10,
  CDROM_CMD_GETLOCP   = 0x11,
  CDROM_CMD_READT     = 0x12,
  CDROM_CMD_GETTN     = 0x13,
  CDROM_CMD_GETTD     = 0x14,
  CDROM_CMD_SEEKL     = 0x15,
  CDROM_CMD_SEEKP     = 0x16,
  CDROM_CMD_SETCLOCK  = 0x17,
  CDROM_CMD_GETCLOCK  = 0x18,
  CDROM_CMD_TEST      = 0x19,
  CDROM_CMD_GETID     = 0x1A,
  CDROM_CMD_READS     = 0x1B,
  CDROM_CMD_RESET     = 0x1C,
  CDROM_CMD_GETQ      = 0x1D,
  CDROM_CMD_READTOC   = 0x1E,
  CDROM_CMD_VIDEOCD   = 0x1F,

  CDROM_CMD_NONE      = 0xFFFF,
} cdrom_command_t;

typedef enum : u8 {
  CDROM_DRV_IDLE                = 0,
  CDROM_DRV_SHELL_OPENING       = 1,
  CDROM_DRV_UNUSED_RESETTING    = 2,
  CDROM_DRV_SEEKING_PHYSICAL    = 3,
  CDROM_DRV_SEEKING_LOGICAL     = 4,
  CDROM_DRV_UNUSED_READING_ID   = 5,
  CDROM_DRV_UNUSED_READING_TOC  = 6,
  CDROM_DRV_READING             = 7,
  CDROM_DRV_PLAYING             = 8,
  CDROM_DRV_UNUSED_PAUSING      = 9,
  CDROM_DRV_UNUSED_STOPPING     = 10,
  CDROM_DRV_CHANGING_SESSION    = 11,
  CDROM_DRV_SPINNING_UP         = 12,
  CDROM_DRV_SEEKING_IMPLICIT    = 13,
  CDROM_DRV_CHANGING_SPEED_OR_TOC_READ = 14,
} cdrom_drive_state_t;

/* StatusRegister at I/O offset 0; index/ADPBUSY/PRMEMPTY/etc. */
typedef union {
  u8 raw;
  struct {
    u8 index   : 2;
    u8 ADPBUSY : 1;
    u8 PRMEMPTY: 1;
    u8 PRMWRDY : 1;
    u8 RSLRRDY : 1;
    u8 DRQSTS  : 1;
    u8 BUSYSTS : 1;
  } bits;
} cdrom_status_register_t;

enum cdrom_stat_bits {
  CDROM_STAT_ERROR        = (1u << 0),
  CDROM_STAT_MOTOR_ON     = (1u << 1),
  CDROM_STAT_SEEK_ERROR   = (1u << 2),
  CDROM_STAT_ID_ERROR     = (1u << 3),
  CDROM_STAT_SHELL_OPEN   = (1u << 4),
  CDROM_STAT_READING      = (1u << 5),
  CDROM_STAT_SEEKING      = (1u << 6),
  CDROM_STAT_PLAYING_CDDA = (1u << 7),
};

enum cdrom_error_reason {
  CDROM_ERR_INVALID_ARGUMENT             = 0x10,
  CDROM_ERR_INCORRECT_NUMBER_OF_PARAMETERS = 0x20,
  CDROM_ERR_INVALID_COMMAND              = 0x40,
  CDROM_ERR_NOT_READY                    = 0x80,
};

typedef union {
  u8 raw;
  struct {
    u8 error        : 1;
    u8 motor_on     : 1;
    u8 seek_error   : 1;
    u8 id_error     : 1;
    u8 shell_open   : 1;
    u8 reading      : 1;
    u8 seeking      : 1;
    u8 playing_cdda : 1;
  } bits;
} cdrom_secondary_status_register_t;

typedef union {
  u8 raw;
  struct {
    u8 cdda            : 1;
    u8 auto_pause      : 1;
    u8 report_audio    : 1;
    u8 xa_filter       : 1;
    u8 ignore_bit      : 1;
    u8 read_raw_sector : 1;
    u8 xa_enable       : 1;
    u8 double_speed    : 1;
  } bits;
} cdrom_mode_register_t;

typedef union {
  u8 raw;
  struct {
    u8 _r0  : 5;
    u8 SMEN : 1;
    u8 BFWR : 1;
    u8 BFRD : 1;
  } bits;
} cdrom_request_register_t;

/* XA Sub-header: file/channel/submode/codinginfo. */
typedef union {
  u8 raw;
  struct {
    u8 eor      : 1;
    u8 video    : 1;
    u8 audio    : 1;
    u8 data     : 1;
    u8 trigger  : 1;
    u8 form2    : 1;
    u8 realtime : 1;
    u8 eof      : 1;
  } bits;
} cdrom_xa_submode_t;

typedef union {
  u8 raw;
  struct {
    u8 mono_stereo     : 1; /* 0=mono, 1=stereo */
    u8 _r1             : 1;
    u8 sample_rate     : 1; /* 0=37800Hz, 1=18900Hz */
    u8 _r3             : 1;
    u8 bits_per_sample : 1; /* 0=4-bit, 1=8-bit */
    u8 _r5             : 1;
    u8 emphasis        : 1;
    u8 _r7             : 1;
  } bits;
} cdrom_xa_codinginfo_t;

typedef struct {
  u8                    file_number;
  u8                    channel_number;
  cdrom_xa_submode_t    submode;
  cdrom_xa_codinginfo_t codinginfo;
} cdrom_xa_subheader_t;

_Static_assert(sizeof(cdrom_xa_subheader_t) == 4, "xa subheader is 4 bytes");

static inline bool xa_codinginfo_is_stereo(cdrom_xa_codinginfo_t c)        { return c.bits.mono_stereo != 0; }
static inline bool xa_codinginfo_is_half_rate(cdrom_xa_codinginfo_t c)     { return c.bits.sample_rate != 0; }
static inline bool xa_codinginfo_is_8bit(cdrom_xa_codinginfo_t c)          { return c.bits.bits_per_sample != 0; }
static inline u32  xa_codinginfo_samples_per_sector(cdrom_xa_codinginfo_t c)
{
  return c.bits.bits_per_sample ? CDROM_XA_ADPCM_SAMPLES_PER_SECTOR_8BIT
                                : CDROM_XA_ADPCM_SAMPLES_PER_SECTOR_4BIT;
}

typedef struct {
  u8  data[CDROM_PARAM_FIFO_SIZE];
  u8  head;  /* next read */
  u8  tail;  /* next write */
  u8  count;
} cdrom_byte_fifo_t;

static inline void byte_fifo_clear(cdrom_byte_fifo_t* f) { f->head = 0; f->tail = 0; f->count = 0; }
static inline bool byte_fifo_is_empty(const cdrom_byte_fifo_t* f) { return f->count == 0; }
static inline bool byte_fifo_is_full(const cdrom_byte_fifo_t* f) { return f->count >= CDROM_PARAM_FIFO_SIZE; }
static inline u32  byte_fifo_size(const cdrom_byte_fifo_t* f) { return f->count; }

static inline void byte_fifo_push(cdrom_byte_fifo_t* f, u8 v)
{
  f->data[f->tail] = v;
  f->tail = (u8)((f->tail + 1u) % CDROM_PARAM_FIFO_SIZE);
  f->count++;
}

static inline u8 byte_fifo_pop(cdrom_byte_fifo_t* f)
{
  u8 v = f->data[f->head];
  f->head = (u8)((f->head + 1u) % CDROM_PARAM_FIFO_SIZE);
  f->count--;
  return v;
}

static inline u8 byte_fifo_peek(const cdrom_byte_fifo_t* f, u32 i)
{
  return f->data[(f->head + i) % CDROM_PARAM_FIFO_SIZE];
}

static inline void byte_fifo_remove_one(cdrom_byte_fifo_t* f)
{
  if (f->count == 0) return;
  f->head = (u8)((f->head + 1u) % CDROM_PARAM_FIFO_SIZE);
  f->count--;
}

static inline void byte_fifo_push_range(cdrom_byte_fifo_t* f, const u8* data, u32 size)
{
  for (u32 i = 0; i < size; i++) byte_fifo_push(f, data[i]);
}

static inline void byte_fifo_push_from_other(cdrom_byte_fifo_t* dst, cdrom_byte_fifo_t* src)
{
  while (!byte_fifo_is_empty(src))
    byte_fifo_push(dst, byte_fifo_pop(src));
}

static void byte_fifo_do_state(state_wrapper_t* sw, cdrom_byte_fifo_t* f)
{
  /* Save format: u32 count, then `count` bytes serialized
   * head-to-tail.  Re-flatten into a fresh buffer on read. */
  u32 size = f->count;
  state_wrapper_do_u32(sw, &size);
  if (state_wrapper_is_reading(sw)) {
    if (size > CDROM_PARAM_FIFO_SIZE) size = CDROM_PARAM_FIFO_SIZE;
    byte_fifo_clear(f);
    for (u32 i = 0; i < size; i++) {
      u8 v = 0;
      state_wrapper_do_u8(sw, &v);
      byte_fifo_push(f, v);
    }
  } else {
    for (u32 i = 0; i < size; i++) {
      u8 v = byte_fifo_peek(f, i);
      state_wrapper_do_u8(sw, &v);
    }
  }
}

typedef struct {
  u32* data;
  u32  capacity;
  u32  head;
  u32  tail;
  u32  count;
} cdrom_audio_fifo_t;

static void audio_fifo_init(cdrom_audio_fifo_t* f)
{
  f->capacity = CDROM_AUDIO_FIFO_SIZE;
  f->data = (u32*)calloc(f->capacity, sizeof(u32));
  f->head = f->tail = f->count = 0;
}

static void audio_fifo_destroy(cdrom_audio_fifo_t* f)
{
  free(f->data); f->data = NULL; f->capacity = 0; f->head = f->tail = f->count = 0;
}

static inline void audio_fifo_clear(cdrom_audio_fifo_t* f) { f->head = 0; f->tail = 0; f->count = 0; }
static inline bool audio_fifo_is_empty(const cdrom_audio_fifo_t* f) { return f->count == 0; }
static inline u32  audio_fifo_size(const cdrom_audio_fifo_t* f) { return f->count; }
static inline u32  audio_fifo_space(const cdrom_audio_fifo_t* f) { return f->capacity - f->count; }

static inline void audio_fifo_push(cdrom_audio_fifo_t* f, u32 v)
{
  if (f->count >= f->capacity) {
    /* drop oldest */
    f->head = (f->head + 1u) % f->capacity;
    f->count--;
  }
  f->data[f->tail] = v;
  f->tail = (f->tail + 1u) % f->capacity;
  f->count++;
}

static inline u32 audio_fifo_pop(cdrom_audio_fifo_t* f)
{
  if (f->count == 0) return 0;
  u32 v = f->data[f->head];
  f->head = (f->head + 1u) % f->capacity;
  f->count--;
  return v;
}

static inline void audio_fifo_remove(cdrom_audio_fifo_t* f, u32 n)
{
  if (n > f->count) n = f->count;
  f->head = (f->head + n) % f->capacity;
  f->count -= n;
}

static void audio_fifo_do_state(state_wrapper_t* sw, cdrom_audio_fifo_t* f)
{
  u32 size = f->count;
  state_wrapper_do_u32(sw, &size);
  if (state_wrapper_is_reading(sw)) {
    if (size > f->capacity) size = f->capacity;
    audio_fifo_clear(f);
    for (u32 i = 0; i < size; i++) {
      u32 v = 0;
      state_wrapper_do_u32(sw, &v);
      audio_fifo_push(f, v);
    }
  } else {
    /* iterate in order */
    u32 idx = f->head;
    for (u32 i = 0; i < size; i++) {
      u32 v = f->data[idx];
      state_wrapper_do_u32(sw, &v);
      idx = (idx + 1u) % f->capacity;
    }
  }
}

typedef struct {
  u8  data[CDROM_RAW_SECTOR_OUTPUT_SIZE];
  u32 position;
  u32 size;
} cdrom_sector_buffer_t;

typedef struct {
  timing_event_t command_event;
  timing_event_t command_second_response_event;
  timing_event_t async_interrupt_event;
  timing_event_t drive_event;

  cdrom_subq_replacement_t subq_replacement;
  bool                     has_subq_replacement;

  global_ticks_t subq_lba_update_tick;
  global_ticks_t last_interrupt_time;

  cdrom_command_t     command;
  cdrom_command_t     command_second_response;
  cdrom_drive_state_t drive_state;
  disc_region_t       disc_region;

  cdrom_status_register_t           status;
  cdrom_secondary_status_register_t secondary_status;
  cdrom_mode_register_t             mode;
  cdrom_request_register_t          request_register;

  u8 interrupt_enable_register;
  u8 interrupt_flag_register;
  u8 pending_async_interrupt;

  bool setloc_pending;
  bool read_after_seek;
  bool play_after_seek;

  cd_image_position_t setloc_position;
  cd_image_lba_t      requested_lba;
  cd_image_lba_t      current_lba;
  cd_image_lba_t      current_subq_lba;
  cd_image_lba_t      seek_start_lba;
  cd_image_lba_t      seek_end_lba;
  u32                 subq_lba_update_carry;

  bool muted;
  bool adpcm_muted;

  u8                    xa_filter_file_number;
  u8                    xa_filter_channel_number;
  u8                    xa_current_file_number;
  u8                    xa_current_channel_number;
  bool                  xa_current_set;
  cdrom_xa_codinginfo_t xa_current_codinginfo;

  cd_image_subq_t          last_subq;
  cd_image_sector_header_t last_sector_header;
  cdrom_xa_subheader_t     last_sector_subheader;
  bool                     last_sector_header_valid;
  bool                     last_subq_needs_update;

  bool cdda_auto_pause_pending;
  u8   cdda_report_start_delay;
  u8   last_cdda_report_frame_nibble;
  u8   play_track_number_bcd;
  u8   async_command_parameter;
  s8   fast_forward_rate;

  u8 cd_audio_volume_matrix[2][2];
  u8 next_cd_audio_volume_matrix[2][2];

  s32 xa_last_samples[4];
  s16 xa_resample_ring_buffer[2][CDROM_XA_RESAMPLE_RING_BUFFER_SIZE];
  u8  xa_resample_p;
  u8  xa_resample_sixstep;

  cdrom_byte_fifo_t param_fifo;
  cdrom_byte_fifo_t response_fifo;
  cdrom_byte_fifo_t async_response_fifo;

  xorshift128pp_t prng;

  cdrom_sector_buffer_t sector_buffers[CDROM_NUM_SECTOR_BUFFERS];
  u32                   current_read_sector_buffer;
  u32                   current_write_sector_buffer;

  cdrom_audio_fifo_t audio_fifo;
} cdrom_state_t;

ALIGN_TO_CACHE_LINE static cdrom_state_t s_state;
ALIGN_TO_CACHE_LINE static cdrom_async_reader_t s_reader;

static const char s_command_event_name[]                 = "CDROM Command Event";
static const char s_command_second_response_event_name[] = "CDROM Command Second Response Event";
static const char s_async_interrupt_event_name[]         = "CDROM Async Interrupt Event";
static const char s_drive_event_name[]                   = "CDROM Drive Event";

/* Drive-state names: kept around for debug logging.  Referenced in a
 * compile-time __attribute__ trick below to silence -Wunused-const-variable
 * on builds that don't enable verbose tracing. */
static const char* const s_drive_state_names[15] = {
  "Idle", "Opening Shell", "Resetting", "Seeking (Physical)", "Seeking (Logical)", "Reading ID",
  "Reading TOC", "Reading", "Playing", "Pausing", "Stopping", "Changing Session", "Spinning Up",
  "Seeking (Implicit)", "Changing Speed/Implicit TOC Read", 
};

/* Forces a use; the compiler still folds the table away if no caller hits it. */
static const char* const* cdrom_get_drive_state_names(void) { return s_drive_state_names; }

static bool cdrom_trace_enabled(void)
{
  return getenv("CUPID_TRACE_CD") != NULL;
}

 /* Temporary trace helper; gated on LOG_LEVEL_DEV (CUPID_TRACE=1).  Fires
 * whenever drive_state changes, so we can correlate state-machine pauses
 * against command issuance / IRQ delivery in trace logs. */
static inline void cdrom_trace_drive_state(const char* where, cdrom_drive_state_t prev, cdrom_drive_state_t now)
{
  if (prev == now) return;
  if (!cdrom_trace_enabled()) return;
  DEV_LOG("[trace] drive_state %s: %s -> %s",
          where,
          (unsigned)prev < 15u ? s_drive_state_names[prev] : "?",
          (unsigned)now  < 15u ? s_drive_state_names[now]  : "?");
}

typedef struct {
  const char* name;
  u8 min_parameters;
  u8 max_parameters;
} cdrom_command_info_t;

/* Per-byte command lookup; 0..0x1F are real, the rest are "Unknown". */
static const cdrom_command_info_t s_command_info[256] = {
  /* 0x00 */ {"Sync", 0, 0},      {"Getstat", 0, 0},   {"Setloc", 3, 3},   {"Play", 0, 1},
  /* 0x04 */ {"Forward", 0, 0},   {"Backward", 0, 0},  {"ReadN", 0, 0},    {"Standby", 0, 0},
  /* 0x08 */ {"Stop", 0, 0},      {"Pause", 0, 0},     {"Init", 0, 0},     {"Mute", 0, 0},
  /* 0x0C */ {"Demute", 0, 0},    {"Setfilter", 2, 2}, {"Setmode", 1, 1},  {"Getmode", 0, 0},
  /* 0x10 */ {"GetlocL", 0, 0},   {"GetlocP", 0, 0},   {"ReadT", 1, 1},    {"GetTN", 0, 0},
  /* 0x14 */ {"GetTD", 1, 1},     {"SeekL", 0, 0},     {"SeekP", 0, 0},    {"SetClock", 0, 0},
  /* 0x18 */ {"GetClock", 0, 0},  {"Test", 1, 16},     {"GetID", 0, 0},    {"ReadS", 0, 0},
  /* 0x1C */ {"Reset", 0, 0},     {"GetQ", 2, 2},      {"ReadTOC", 0, 0},  {"VideoCD", 6, 16},
  /* 0x20 .. 0xFF default-init to {NULL, 0, 0}, treated as Unknown */
};

static tick_count_t cdrom_soft_reset(tick_count_t ticks_late);
static const cd_image_subq_t* cdrom_get_sector_subq(u32 lba, const cd_image_subq_t* real_subq);
static bool cdrom_can_read_media(void);
static bool cdrom_is_drive_idle(void);
static bool cdrom_is_motor_on(void);
static bool cdrom_is_seeking(void);
static bool cdrom_is_reading(void);
static bool cdrom_is_reading_or_playing(void);
static bool cdrom_has_pending_command(void);
static bool cdrom_has_pending_interrupt(void);
static bool cdrom_has_pending_async_interrupt(void);
static void cdrom_add_audio_frame(s16 left, s16 right);
static s32  cdrom_apply_volume(s16 sample, u8 volume);
static s16  cdrom_saturate_volume(s32 volume);
static void cdrom_set_interrupt(cdrom_interrupt_t interrupt);
static void cdrom_set_async_interrupt(cdrom_interrupt_t interrupt);
static void cdrom_clear_async_interrupt(void);
static void cdrom_deliver_async_interrupt(void* user, tick_count_t ticks, tick_count_t ticks_late);
static void cdrom_queue_deliver_async_interrupt(void);
static void cdrom_send_ack_and_stat(void);
static void cdrom_send_error_response(u8 stat_bits, u8 reason);
static void cdrom_send_async_error_response(u8 stat_bits, u8 reason);
static void cdrom_update_status_register(void);
static void cdrom_update_interrupt_request(void);
static bool cdrom_has_pending_disc_event(void);
static bool cdrom_can_use_read_speedup(void);

static tick_count_t cdrom_get_ack_delay_for_command(cdrom_command_t cmd);
static tick_count_t cdrom_get_ticks_for_spin_up(void);
static tick_count_t cdrom_get_ticks_for_id_read(void);
static tick_count_t cdrom_get_ticks_for_read(void);
static tick_count_t cdrom_get_ticks_for_seek(cd_image_lba_t new_lba, bool ignore_speed_change);
static tick_count_t cdrom_get_ticks_for_pause(void);
static tick_count_t cdrom_get_ticks_for_stop(bool motor_was_on);
static tick_count_t cdrom_get_ticks_for_speed_change(void);
static tick_count_t cdrom_get_ticks_for_toc_read(void);
static cd_image_lba_t cdrom_get_next_sector_to_be_read(void);
static u32  cdrom_get_sectors_per_track(cd_image_lba_t lba);
static bool cdrom_complete_seek(void);

static void cdrom_begin_command(cdrom_command_t command);
static void cdrom_end_command(void);
static void cdrom_execute_command(void* user, tick_count_t ticks, tick_count_t ticks_late);
static void cdrom_execute_test_command(u8 subcommand);
static void cdrom_execute_command_second_response(void* user, tick_count_t ticks, tick_count_t ticks_late);
static void cdrom_queue_command_second_response(cdrom_command_t cmd, tick_count_t ticks);
static void cdrom_clear_command_second_response(void);
static void cdrom_update_command_event(void);
static void cdrom_execute_drive(void* user, tick_count_t ticks, tick_count_t ticks_late);
static void cdrom_clear_drive_state(void);
static void cdrom_begin_reading(tick_count_t ticks_late, bool after_seek);
static void cdrom_begin_playing(u8 track, tick_count_t ticks_late, bool after_seek);
static void cdrom_do_shell_open_complete(tick_count_t ticks_late);
static void cdrom_do_seek_complete(tick_count_t ticks_late);
static void cdrom_do_stat_second_response(void);
static void cdrom_do_change_session_complete(void);
static void cdrom_do_spin_up_complete(void);
static void cdrom_do_speed_change_or_implicit_toc_read_complete(void);
static void cdrom_do_id_read(void);
static void cdrom_do_sector_read(void);
static void cdrom_process_data_sector_header(const u8* raw_sector);
static void cdrom_process_data_sector(const u8* raw_sector, const cd_image_subq_t* subq);
static void cdrom_process_xa_adpcm_sector(const u8* raw_sector, const cd_image_subq_t* subq);
static void cdrom_process_cdda_sector(const u8* raw_sector, const cd_image_subq_t* subq, bool subq_valid);
static void cdrom_stop_reading_with_data_end(void);
static void cdrom_stop_reading_with_error(u8 reason);
static void cdrom_start_motor(void);
static void cdrom_stop_motor(void);
static void cdrom_begin_seeking(bool logical, bool read_after_seek_, bool play_after_seek_);
static void cdrom_update_subq_position_while_seeking(void);
static void cdrom_update_subq_position(bool update_logical);
static void cdrom_ensure_last_subq_valid(void);
static void cdrom_set_hold_position(cd_image_lba_t lba, cd_image_lba_t subq_lba);
static void cdrom_reset_current_xa_file(void);
static void cdrom_reset_audio_decoder(void);
static void cdrom_clear_sector_buffers(void);
static void cdrom_check_for_sector_buffer_read_complete(void);

static void cdrom_decode_xa_adpcm_chunks(const u8* chunk_ptr, s16* samples, bool is_stereo, bool is_8bit);
static void cdrom_resample_xa_adpcm(const s16* frames_in, u32 num_frames_in, bool stereo);
static void cdrom_resample_xa_adpcm_18900(const s16* frames_in, u32 num_frames_in, bool stereo);

static bool cdrom_is_drive_idle(void) { return s_state.drive_state == CDROM_DRV_IDLE; }
static bool cdrom_is_motor_on(void)   { return s_state.secondary_status.bits.motor_on != 0; }
static bool cdrom_is_seeking(void)
{
  return s_state.drive_state == CDROM_DRV_SEEKING_LOGICAL ||
         s_state.drive_state == CDROM_DRV_SEEKING_PHYSICAL ||
         s_state.drive_state == CDROM_DRV_SEEKING_IMPLICIT;
}
static bool cdrom_is_reading(void) { return s_state.drive_state == CDROM_DRV_READING; }
static bool cdrom_is_reading_or_playing(void)
{
  return s_state.drive_state == CDROM_DRV_READING || s_state.drive_state == CDROM_DRV_PLAYING;
}
static bool cdrom_can_read_media(void)
{
  return s_state.drive_state != CDROM_DRV_SHELL_OPENING && cdrom_async_reader_has_media(&s_reader);
}
static bool cdrom_has_pending_command(void)         { return s_state.command != CDROM_CMD_NONE; }
static bool cdrom_has_pending_interrupt(void)       { return s_state.interrupt_flag_register != 0; }
static bool cdrom_has_pending_async_interrupt(void) { return s_state.pending_async_interrupt != 0; }

static inline void cdrom_secondary_clear_active_bits(void)
{
  s_state.secondary_status.raw &= (u8)~(CDROM_STAT_SEEKING | CDROM_STAT_READING | CDROM_STAT_PLAYING_CDDA);
}

static inline void cdrom_secondary_set_seeking(void)
{
  s_state.secondary_status.raw =
    (u8)((s_state.secondary_status.raw & (u8)~(CDROM_STAT_READING | CDROM_STAT_PLAYING_CDDA)) | 
         (CDROM_STAT_MOTOR_ON | CDROM_STAT_SEEKING));
}

static inline void cdrom_secondary_set_reading_bits(bool audio)
{
  s_state.secondary_status.raw =
    (u8)((s_state.secondary_status.raw & (u8)~(CDROM_STAT_SEEKING | CDROM_STAT_READING | CDROM_STAT_PLAYING_CDDA)) |
         (audio ? (CDROM_STAT_MOTOR_ON | CDROM_STAT_PLAYING_CDDA) 
                : (CDROM_STAT_MOTOR_ON | CDROM_STAT_READING)));
}

void cdrom_initialize(void)
{
  cdrom_async_reader_init(&s_reader);
  audio_fifo_init(&s_state.audio_fifo);
  cdrom_subq_replacement_init(&s_state.subq_replacement);
  s_state.has_subq_replacement = false;
  s_state.disc_region = DISC_REGION_NON_PS1;

  timing_event_init(&s_state.command_event,
                    s_command_event_name, (u32)(sizeof(s_command_event_name) - 1u),
                    1, 1, &cdrom_execute_command, NULL);
  timing_event_init(&s_state.command_second_response_event,
                     s_command_second_response_event_name,
                    (u32)(sizeof(s_command_second_response_event_name) - 1u), 
                    1, 1, &cdrom_execute_command_second_response, NULL);
  timing_event_init(&s_state.async_interrupt_event,
                    s_async_interrupt_event_name, (u32)(sizeof(s_async_interrupt_event_name) - 1u),
                    CDROM_INTERRUPT_DELAY_CYCLES, 1, &cdrom_deliver_async_interrupt, NULL);
  timing_event_init(&s_state.drive_event,
                    s_drive_event_name, (u32)(sizeof(s_drive_event_name) - 1u),
                    1, 1, &cdrom_execute_drive, NULL);

  if (g_settings.cdrom_readahead_sectors > 0)
    cdrom_async_reader_start_thread(&s_reader, g_settings.cdrom_readahead_sectors);

  cdrom_reset();
}

void cdrom_shutdown(void)
{
  timing_event_deactivate(&s_state.drive_event);
  timing_event_deactivate(&s_state.async_interrupt_event);
  timing_event_deactivate(&s_state.command_second_response_event);
  timing_event_deactivate(&s_state.command_event);

  timing_event_destroy(&s_state.drive_event);
  timing_event_destroy(&s_state.async_interrupt_event);
  timing_event_destroy(&s_state.command_second_response_event);
  timing_event_destroy(&s_state.command_event);

  cdrom_async_reader_stop_thread(&s_reader);
  cd_image_t* prev = cdrom_async_reader_remove_media(&s_reader);
  if (prev) cd_image_destroy(prev);
  cdrom_async_reader_destroy(&s_reader);

  cdrom_subq_replacement_destroy(&s_state.subq_replacement);
  s_state.has_subq_replacement = false;
  audio_fifo_destroy(&s_state.audio_fifo);
}

void cdrom_reset(void)
{
  s_state.command = CDROM_CMD_NONE;
  timing_event_deactivate(&s_state.command_event);
  cdrom_clear_command_second_response();
  cdrom_clear_drive_state();
  s_state.status.raw = 0;
  s_state.secondary_status.raw = 0;
  s_state.secondary_status.bits.motor_on   = cdrom_can_read_media();
  s_state.secondary_status.bits.shell_open = !cdrom_can_read_media();
  s_state.mode.raw = 0;
  s_state.mode.bits.read_raw_sector = 1;
  s_state.interrupt_enable_register = CDROM_INTERRUPT_REGISTER_MASK;
  s_state.interrupt_flag_register = 0;
  s_state.last_interrupt_time =
    timing_events_get_global_tick_counter() - CDROM_MINIMUM_INTERRUPT_DELAY;
  cdrom_clear_async_interrupt();
  memset(&s_state.setloc_position, 0, sizeof(s_state.setloc_position));
  s_state.seek_start_lba = 0;
  s_state.seek_end_lba = 0;
  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
  s_state.muted = false;
  s_state.adpcm_muted = false;
  s_state.xa_filter_file_number = 0;
  s_state.xa_filter_channel_number = 0;
  s_state.xa_current_file_number = 0;
  s_state.xa_current_channel_number = 0;
  s_state.xa_current_set = false;
  memset(&s_state.last_sector_header, 0, sizeof(s_state.last_sector_header));
  memset(&s_state.last_sector_subheader, 0, sizeof(s_state.last_sector_subheader));
  s_state.last_sector_header_valid = false;
  memset(&s_state.last_subq, 0, sizeof(s_state.last_subq));
  s_state.cdda_report_start_delay = 0;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  s_state.next_cd_audio_volume_matrix[0][0] = 0x80;
  s_state.next_cd_audio_volume_matrix[0][1] = 0x00;
  s_state.next_cd_audio_volume_matrix[1][0] = 0x00;
  s_state.next_cd_audio_volume_matrix[1][1] = 0x80;
  memcpy(s_state.cd_audio_volume_matrix, s_state.next_cd_audio_volume_matrix,
         sizeof(s_state.cd_audio_volume_matrix));

  cdrom_clear_sector_buffers();
  cdrom_reset_audio_decoder();

  byte_fifo_clear(&s_state.param_fifo);
  byte_fifo_clear(&s_state.response_fifo);
  byte_fifo_clear(&s_state.async_response_fifo);
  xorshift128pp_seed(&s_state.prng, CDROM_PRNG_SEED);

  cdrom_update_status_register();

  cdrom_set_hold_position(0, 0);
}

static tick_count_t cdrom_soft_reset(tick_count_t ticks_late)
{
  const bool was_double_speed = s_state.mode.bits.double_speed != 0;

  cdrom_clear_command_second_response();
  cdrom_clear_drive_state();
  s_state.secondary_status.raw = 0;
  s_state.secondary_status.bits.motor_on   = cdrom_can_read_media();
  s_state.secondary_status.bits.shell_open = !cdrom_can_read_media();
  s_state.mode.raw = 0;
  s_state.mode.bits.read_raw_sector = 1;
  s_state.request_register.raw = 0;
  cdrom_clear_async_interrupt();
  memset(&s_state.setloc_position, 0, sizeof(s_state.setloc_position));
  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
  s_state.muted = false;
  s_state.adpcm_muted = false;
  s_state.cdda_auto_pause_pending = false;
  s_state.cdda_report_start_delay = 0;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  cdrom_clear_sector_buffers();
  cdrom_reset_audio_decoder();

  byte_fifo_clear(&s_state.param_fifo);
  byte_fifo_clear(&s_state.async_response_fifo);

  cdrom_update_status_register();

  tick_count_t total_ticks;
  if (cdrom_has_media()) {
    if (cdrom_is_seeking())
      cdrom_update_subq_position_while_seeking();
    else
      cdrom_update_subq_position(false);

    const tick_count_t speed_change_ticks = was_double_speed ? cdrom_get_ticks_for_speed_change() : 0;
    const tick_count_t seek_ticks = (s_state.current_lba != 0) ? cdrom_get_ticks_for_seek(0, false) : 0;
    tick_count_t want = speed_change_ticks + seek_ticks;
    if (want < CDROM_INIT_TICKS) want = CDROM_INIT_TICKS;
    total_ticks = want - ticks_late;

    if (s_state.current_lba != 0) {
      s_state.drive_state = CDROM_DRV_SEEKING_IMPLICIT;
      timing_event_set_interval_and_schedule(&s_state.drive_event, total_ticks);
      s_state.requested_lba = 0;
      cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
      s_state.seek_start_lba = s_state.current_lba;
      s_state.seek_end_lba = 0;
    } else {
      s_state.drive_state = CDROM_DRV_CHANGING_SPEED_OR_TOC_READ;
      timing_event_schedule(&s_state.drive_event, total_ticks);
    }
  } else {
    total_ticks = CDROM_INIT_TICKS - ticks_late;
  }

  return total_ticks;
}

bool cdrom_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_array(sw, &s_state.command, sizeof(s_state.command), 1);
  /* command_second_response added at version 53; default is None.  Older
   * states won't have a meaningful value here, so on read at version<53 we
   * leave it as None. */
  if (state_wrapper_get_version(sw) >= 53) {
    state_wrapper_do_array(sw, &s_state.command_second_response,
                           sizeof(s_state.command_second_response), 1);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.command_second_response = CDROM_CMD_NONE;
  }
  state_wrapper_do_array(sw, &s_state.drive_state, sizeof(s_state.drive_state), 1);
  state_wrapper_do_u8(sw, &s_state.status.raw);
  state_wrapper_do_u8(sw, &s_state.secondary_status.raw);
  state_wrapper_do_u8(sw, &s_state.mode.raw);
  if (state_wrapper_get_version(sw) >= 65) {
    state_wrapper_do_u8(sw, &s_state.request_register.raw);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.request_register.raw = 0;
  }

  bool current_double_speed = (s_state.mode.bits.double_speed != 0);
  state_wrapper_do_bool(sw, &current_double_speed);

  state_wrapper_do_u8(sw, &s_state.interrupt_enable_register);
  state_wrapper_do_u8(sw, &s_state.interrupt_flag_register);

  if (state_wrapper_get_version(sw) < 71) {
    u32 last_interrupt_time32 = 0;
    if (state_wrapper_get_version(sw) >= 57) {
      state_wrapper_do_u32(sw, &last_interrupt_time32);
    } else if (state_wrapper_is_reading(sw)) {
      last_interrupt_time32 = (u32)(timing_events_get_global_tick_counter() - CDROM_MINIMUM_INTERRUPT_DELAY);
    }
    s_state.last_interrupt_time = last_interrupt_time32;
  } else {
    state_wrapper_do_u64(sw, &s_state.last_interrupt_time);
  }

  state_wrapper_do_u8(sw, &s_state.pending_async_interrupt);
  state_wrapper_do_array(sw, &s_state.setloc_position, sizeof(s_state.setloc_position), 1);
  state_wrapper_do_u32(sw, &s_state.current_lba);
  state_wrapper_do_u32(sw, &s_state.seek_start_lba);
  state_wrapper_do_u32(sw, &s_state.seek_end_lba);
  if (state_wrapper_get_version(sw) >= 49) {
    state_wrapper_do_u32(sw, &s_state.current_subq_lba);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.current_subq_lba = s_state.current_lba;
  }

  if (state_wrapper_get_version(sw) < 71) {
    u32 subq_lba_update_tick32 = 0;
    if (state_wrapper_get_version(sw) >= 49) {
      state_wrapper_do_u32(sw, &subq_lba_update_tick32);
    }
    s_state.subq_lba_update_tick = subq_lba_update_tick32;
  } else {
    state_wrapper_do_u64(sw, &s_state.subq_lba_update_tick);
  }

  if (state_wrapper_get_version(sw) >= 54) {
    state_wrapper_do_u32(sw, &s_state.subq_lba_update_carry);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.subq_lba_update_carry = 0;
  }
  state_wrapper_do_bool(sw, &s_state.setloc_pending);
  state_wrapper_do_bool(sw, &s_state.read_after_seek);
  state_wrapper_do_bool(sw, &s_state.play_after_seek);
  state_wrapper_do_bool(sw, &s_state.muted);
  state_wrapper_do_bool(sw, &s_state.adpcm_muted);
  state_wrapper_do_u8(sw, &s_state.xa_filter_file_number);
  state_wrapper_do_u8(sw, &s_state.xa_filter_channel_number);
  state_wrapper_do_u8(sw, &s_state.xa_current_file_number);
  state_wrapper_do_u8(sw, &s_state.xa_current_channel_number);
  state_wrapper_do_bool(sw, &s_state.xa_current_set);
  state_wrapper_do_bytes(sw, &s_state.last_sector_header, sizeof(s_state.last_sector_header));
  state_wrapper_do_bytes(sw, &s_state.last_sector_subheader, sizeof(s_state.last_sector_subheader));
  state_wrapper_do_bool(sw, &s_state.last_sector_header_valid);
  state_wrapper_do_bytes(sw, &s_state.last_subq, sizeof(s_state.last_subq));
  if (state_wrapper_get_version(sw) >= 81) {
    state_wrapper_do_bool(sw, &s_state.cdda_auto_pause_pending);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.cdda_auto_pause_pending = false;
  }
  if (state_wrapper_get_version(sw) >= 72) {
    state_wrapper_do_u8(sw, &s_state.cdda_report_start_delay);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.cdda_report_start_delay = 0;
  }
  state_wrapper_do_u8(sw, &s_state.last_cdda_report_frame_nibble);
  state_wrapper_do_u8(sw, &s_state.play_track_number_bcd);
  state_wrapper_do_u8(sw, &s_state.async_command_parameter);

  if (state_wrapper_get_version(sw) >= 49) {
    state_wrapper_do_s8(sw, &s_state.fast_forward_rate);
  } else if (state_wrapper_is_reading(sw)) {
    s_state.fast_forward_rate = 0;
  }

  state_wrapper_do_array(sw, s_state.cd_audio_volume_matrix, 1u, 4u);
  state_wrapper_do_array(sw, s_state.next_cd_audio_volume_matrix, 1u, 4u);
  state_wrapper_do_array(sw, s_state.xa_last_samples, sizeof(s32), 4u);
  state_wrapper_do_array(sw, s_state.xa_resample_ring_buffer, sizeof(s16), 2u * CDROM_XA_RESAMPLE_RING_BUFFER_SIZE);
  state_wrapper_do_u8(sw, &s_state.xa_resample_p);
  state_wrapper_do_u8(sw, &s_state.xa_resample_sixstep);
  byte_fifo_do_state(sw, &s_state.param_fifo);
  byte_fifo_do_state(sw, &s_state.response_fifo);
  byte_fifo_do_state(sw, &s_state.async_response_fifo);

  if (state_wrapper_get_version(sw) >= 79) {
    state_wrapper_do_array(sw, &s_state.prng.state, sizeof(s_state.prng.state), 1);
  } else if (state_wrapper_is_reading(sw)) {
    xorshift128pp_seed(&s_state.prng, CDROM_PRNG_SEED);
  }

  if (state_wrapper_get_version(sw) < 65) {
    /* skip "old data fifo": u32 size + size bytes */
    u32 old_fifo_size = 0;
    state_wrapper_do_u32(sw, &old_fifo_size);
    state_wrapper_skip_bytes(sw, old_fifo_size);

    state_wrapper_do_u32(sw, &s_state.current_read_sector_buffer);
    state_wrapper_do_u32(sw, &s_state.current_write_sector_buffer);
    for (u32 i = 0; i < CDROM_NUM_SECTOR_BUFFERS; i++) {
      cdrom_sector_buffer_t* sb = &s_state.sector_buffers[i];
      state_wrapper_do_array(sw, sb->data, 1u, sizeof(sb->data));
      state_wrapper_do_u32(sw, &sb->size);
      sb->position = 0;
    }
    if (state_wrapper_is_reading(sw) && old_fifo_size > 0) {
      cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];
      sb->size = s_state.mode.bits.read_raw_sector ? CDROM_RAW_SECTOR_OUTPUT_SIZE : CDROM_DATA_SECTOR_OUTPUT_SIZE;
      sb->position = (sb->size > old_fifo_size) ? (sb->size - old_fifo_size) : 0;
      s_state.request_register.bits.BFRD = (sb->position > 0) ? 1u : 0u;
    }
    cdrom_update_status_register();
  } else {
    state_wrapper_do_u32(sw, &s_state.current_read_sector_buffer);
    state_wrapper_do_u32(sw, &s_state.current_write_sector_buffer);

    for (u32 i = 0; i < CDROM_NUM_SECTOR_BUFFERS; i++) {
      cdrom_sector_buffer_t* sb = &s_state.sector_buffers[i];
      state_wrapper_do_u32(sw, &sb->size);
      state_wrapper_do_u32(sw, &sb->position);
      if (sb->position < sb->size)
        state_wrapper_do_bytes(sw, &sb->data[sb->position], sb->size - sb->position);
    }
  }

  audio_fifo_do_state(sw, &s_state.audio_fifo);
  state_wrapper_do_u32(sw, &s_state.requested_lba);

  if (state_wrapper_is_reading(sw)) {
    s_state.last_subq_needs_update = true;
    if (cdrom_async_reader_has_media(&s_reader))
      cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
    cdrom_update_command_event();
    timing_event_set_state(&s_state.drive_event, !cdrom_is_drive_idle());
    timing_event_set_state(&s_state.command_second_response_event,
                           s_state.command_second_response != CDROM_CMD_NONE);
  }

  return !state_wrapper_has_error(sw);
}

bool cdrom_has_media(void) { return cdrom_async_reader_has_media(&s_reader); }

const char* cdrom_get_media_path(void) { return cdrom_async_reader_get_media_path(&s_reader); }

u32 cdrom_get_current_sub_image(void)
{
  const cd_image_t* m = cdrom_async_reader_get_media_const(&s_reader);
  return m ? cd_image_get_current_sub_image(m) : 0u;
}

bool cdrom_has_non_standard_or_replacement_subq(void)
{
  const cd_image_t* media = cdrom_async_reader_get_media_const(&s_reader);
  return (media && cd_image_has_subchannel_data(media)) || s_state.has_subq_replacement;
}

const cd_image_t* cdrom_get_media(void) { return cdrom_async_reader_get_media_const(&s_reader); }

disc_region_t cdrom_get_disc_region(void) { return s_state.disc_region; }

bool cdrom_is_media_ps1_disc(void) { return s_state.disc_region != DISC_REGION_NON_PS1; }

bool cdrom_is_media_audio_cd(void)
{
  const cd_image_t* media = cdrom_async_reader_get_media_const(&s_reader);
  if (!media) return false;
  /* Track 1 is index 0 in our tracks array.  Audio == TrackMode::Audio. */
  if (cd_image_get_track_count(media) == 0) return false;
  return cd_image_get_tracks(media)[0].mode == CD_IMAGE_TRACK_MODE_AUDIO;
}

bool cdrom_does_media_region_match_console(void)
{
  if (!g_settings.cdrom_region_check) return true;
  if (s_state.disc_region == DISC_REGION_OTHER) return false;
  return cdrom_get_console_region() == cdrom_console_region_for_disc(s_state.disc_region);
}

bool cdrom_insert_media(cd_image_t* media, disc_region_t region,
                        const char* serial, size_t serial_len,
                        const char* title, size_t title_len,
                        const char* save_title, size_t save_title_len,
                        Error* error)
{
  /* Load SBI/LSD first so we don't accept the disc without sidecar
   * processing succeeding. */
  cdrom_subq_replacement_t subq;
  cdrom_subq_replacement_init(&subq);
  bool has_data = false;
  if (!cd_image_has_subchannel_data(media)) {
    if (!cdrom_subq_replacement_load_for_image(&subq, &has_data, media,
                                               serial, serial_len, title, title_len,
                                               save_title, save_title_len, error)) {
      cdrom_subq_replacement_destroy(&subq);
      return false;
    }
  }

  if (cdrom_can_read_media())
    (void)cdrom_remove_media(true);

  INFO_LOG("Inserting new media, disc region: %u, console region: %u",
           (unsigned)region, (unsigned)cdrom_get_console_region());

  cdrom_subq_replacement_destroy(&s_state.subq_replacement);
  s_state.subq_replacement = subq;
  s_state.has_subq_replacement = has_data;
  s_state.disc_region = region;
  cdrom_async_reader_set_media(&s_reader, media);
  cdrom_set_hold_position(0, 0);

  if (s_state.drive_state != CDROM_DRV_SHELL_OPENING)
    cdrom_start_motor();

  return true;
}

cd_image_t* cdrom_remove_media(bool for_disc_swap)
{
  if (!cdrom_has_media()) return NULL;

  /* Add an extra two seconds to disc swap so games like Metal Gear Solid
   * don't see the new disc immediately. */
  tick_count_t stop_ticks = cdrom_get_ticks_for_stop(true);
  if (for_disc_swap)
    stop_ticks += cdrom_scale_ticks_to_overclock((tick_count_t)(CDROM_MASTER_CLOCK * 2));

  INFO_LOG("Removing CD...");
  cd_image_t* image = cdrom_async_reader_remove_media(&s_reader);

  s_state.last_sector_header_valid = false;

  s_state.secondary_status.bits.motor_on = 0;
  s_state.secondary_status.bits.shell_open = 1;
  cdrom_secondary_clear_active_bits();
  s_state.disc_region = DISC_REGION_NON_PS1;
  cdrom_subq_replacement_destroy(&s_state.subq_replacement);
  cdrom_subq_replacement_init(&s_state.subq_replacement);
  s_state.has_subq_replacement = false;

  cdrom_clear_drive_state();
  cdrom_clear_command_second_response();
  s_state.command = CDROM_CMD_NONE;
  timing_event_deactivate(&s_state.command_event);

  cdrom_clear_async_interrupt();
  cdrom_send_async_error_response(CDROM_STAT_ERROR, 0x08);

  if (for_disc_swap) {
    s_state.drive_state = CDROM_DRV_SHELL_OPENING;
    timing_event_set_interval_and_schedule(&s_state.drive_event, stop_ticks);
  }

  return image;
}

bool cdrom_precache_media(void)
{
  cd_image_t* media = cdrom_async_reader_get_media(&s_reader);
  if (!media) return false;
  return cdrom_async_reader_precache(&s_reader, NULL, NULL);
}

static const cd_image_subq_t* cdrom_get_sector_subq(u32 lba, const cd_image_subq_t* real_subq)
{
  if (s_state.has_subq_replacement) {
    const cd_image_subq_t* repl = cdrom_subq_replacement_get_replacement_subq(&s_state.subq_replacement, lba);
    if (repl) return repl;
  }
  return real_subq;
}

void cdrom_set_readahead_sectors(u32 readahead_sectors)
{
  const bool want_thread = (readahead_sectors > 0);
  if (want_thread == cdrom_async_reader_is_using_thread(&s_reader) &&
      cdrom_async_reader_get_readahead_count(&s_reader) == readahead_sectors)
    return;

  if (want_thread)
    cdrom_async_reader_start_thread(&s_reader, readahead_sectors);
  else
    cdrom_async_reader_stop_thread(&s_reader);

  if (cdrom_has_media())
    cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
}

void cdrom_cpu_clock_changed(void)
{
  /* Reschedule the disc read event with the new tick rate. */
  if (cdrom_is_reading_or_playing())
    timing_event_set_interval(&s_state.drive_event, cdrom_get_ticks_for_read());
}

u8 cdrom_read_register(u32 offset)
{
  switch (offset) {
    case 0:
      TRACE_LOG("CDROM read status register -> 0x%02X", s_state.status.raw);
      return s_state.status.raw;

    case 1: {
      if (byte_fifo_is_empty(&s_state.response_fifo)) {
        DEV_LOG("Response FIFO empty on read");
        return 0x00;
      }
      const u8 value = byte_fifo_pop(&s_state.response_fifo);
      cdrom_update_status_register();
      DEBUG_LOG("CDROM read response FIFO -> 0x%02X", value);
      return value;
    }

    case 2: {
      cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];
      u8 value = 0;
      if (s_state.request_register.bits.BFRD && sb->position < sb->size) {
        value = sb->data[sb->position++];
        cdrom_check_for_sector_buffer_read_complete();
      } else {
        WARNING_LOG("Sector buffer overread (BFRD=%u, buffer=%u, pos=%u, size=%u)",
                    (unsigned)s_state.request_register.bits.BFRD,
                    (unsigned)s_state.current_read_sector_buffer, sb->position, sb->size);
      }
      DEBUG_LOG("CDROM read data FIFO -> 0x%02X", value);
      return value;
    }

    case 3: {
      if (s_state.status.bits.index & 1u) {
        const u8 value = (u8)(s_state.interrupt_flag_register | (u8)~CDROM_INTERRUPT_REGISTER_MASK);
        DEBUG_LOG("CDROM read interrupt flag register -> 0x%02X", value);
        return value;
      } else {
        const u8 value = (u8)(s_state.interrupt_enable_register | (u8)~CDROM_INTERRUPT_REGISTER_MASK);
        DEBUG_LOG("CDROM read interrupt enable register -> 0x%02X", value);
        return value;
      }
    }

    default:
      ERROR_LOG("Unknown CDROM register read: offset=0x%02X, index=%u", offset,
                (unsigned)s_state.status.bits.index);
      return 0;
  }
}

/* Per-byte param-FIFO write trace (forward decls; storage near cmd
 * ringbuf below).  Used by cdrom_write_register's case-1 path to record
 * (value, src_pc, tick), and by the Invalid-seek error in
 * cdrom_execute_command to dump the trace on rejection. */
typedef struct cdrom_dbg_param_trace_s_fwd cdrom_dbg_param_trace_t;
struct cdrom_dbg_param_trace_s_fwd {
  u32 src_pc;
  u64 tick;
  u8  value;
};
#define CDROM_DBG_PARAM_TRACE_CAP 32u
extern cdrom_dbg_param_trace_t cdrom_dbg_param_trace[CDROM_DBG_PARAM_TRACE_CAP];
extern u32 cdrom_dbg_param_trace_pos;
extern u32 cdrom_dbg_param_trace_count;
static void cdrom_dbg_dump_param_trace(void);

void cdrom_write_register(u32 offset, u8 value)
{
  if (offset == 0) {
    TRACE_LOG("CDROM status register <- 0x%02X", value);
    s_state.status.raw = (u8)((s_state.status.raw & (u8)~3u) | (value & 3u));
    return;
  }

  const u32 reg = ((u32)s_state.status.bits.index * 3u) + (offset - 1u);
  switch (reg) {
    case 0:
      DEBUG_LOG("CDROM command register <- 0x%02X (%s)", value,
                s_command_info[value].name ? s_command_info[value].name : "Unknown");
      cdrom_begin_command((cdrom_command_t)value);
      return;

    case 1: {
      /* Record source PC for each param byte pushed.  On a later
       * "Invalid seek" rejection we dump these so the offending sb / sw
       * at the bad PC can be disassembled. */
      cdrom_dbg_param_trace[cdrom_dbg_param_trace_pos].value  = value;
      cdrom_dbg_param_trace[cdrom_dbg_param_trace_pos].src_pc = g_cpu_state.current_instruction_pc;
      cdrom_dbg_param_trace[cdrom_dbg_param_trace_pos].tick   = (u64)timing_events_get_global_tick_counter();
      cdrom_dbg_param_trace_pos = (cdrom_dbg_param_trace_pos + 1u) % CDROM_DBG_PARAM_TRACE_CAP;
      if (cdrom_dbg_param_trace_count < CDROM_DBG_PARAM_TRACE_CAP)
        ++cdrom_dbg_param_trace_count;

      if (byte_fifo_is_full(&s_state.param_fifo)) {
        WARNING_LOG("Parameter FIFO overflow");
        byte_fifo_remove_one(&s_state.param_fifo);
      }
      byte_fifo_push(&s_state.param_fifo, value);
      cdrom_update_status_register();
      return;
    }

    case 2: {
      DEBUG_LOG("Request register <- 0x%02X", value);
      cdrom_request_register_t rr; rr.raw = value;
      if (rr.bits.SMEN) ERROR_LOG("Sound map enable set");
      if (rr.bits.BFWR) ERROR_LOG("Buffer write enable set");
      s_state.request_register.raw = rr.raw;

      cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];
      if (!s_state.request_register.bits.BFRD) {
        /* Clearing BFRD resets the buffer position; some games (Metal Gear
         * Solid: Special Missions PAL) rely on this between DMAs. */
        sb->position = 0;
      } else {
        if (sb->size == 0)
          WARNING_LOG("Setting BFRD without a buffer ready.");
      }
      cdrom_update_status_register();
      return;
    }

    case 3:
      ERROR_LOG("Sound map data out <- 0x%02X", value);
      return;

    case 4:
      DEBUG_LOG("Interrupt enable register <- 0x%02X", value);
      s_state.interrupt_enable_register = value & CDROM_INTERRUPT_REGISTER_MASK;
      cdrom_update_interrupt_request();
      return;

    case 5: {
      DEBUG_LOG("Interrupt flag register <- 0x%02X", value);
      const u8 prev = s_state.interrupt_flag_register;
      s_state.interrupt_flag_register &= (u8)~(value & CDROM_INTERRUPT_REGISTER_MASK);
      if (s_state.interrupt_flag_register == 0) {
        if (prev != 0)
          s_state.last_interrupt_time = timing_events_get_global_tick_counter();
        interrupt_controller_set_line_state(INTERRUPT_CONTROLLER_IRQ_CDROM, false);
        if (cdrom_has_pending_async_interrupt() && !cdrom_has_pending_command())
          cdrom_queue_deliver_async_interrupt();
        else
          cdrom_update_command_event();
      }
      if (value & 0x40u) {
        byte_fifo_clear(&s_state.param_fifo);
        cdrom_update_status_register();
      }
      return;
    }

    case 6:
      ERROR_LOG("Sound map coding info <- 0x%02X", value);
      return;

    case 7:
      DEBUG_LOG("Audio volume L->L <- 0x%02X", value);
      s_state.next_cd_audio_volume_matrix[0][0] = value;
      return;
    case 8:
      DEBUG_LOG("Audio volume L->R <- 0x%02X", value);
      s_state.next_cd_audio_volume_matrix[0][1] = value;
      return;
    case 9:
      DEBUG_LOG("Audio volume R->R <- 0x%02X", value);
      s_state.next_cd_audio_volume_matrix[1][1] = value;
      return;
    case 10:
      DEBUG_LOG("Audio volume R->L <- 0x%02X", value);
      s_state.next_cd_audio_volume_matrix[1][0] = value;
      return;

    case 11: {
      DEBUG_LOG("Audio volume apply changes <- 0x%02X", value);
      const bool adpcm_muted = (value & 0x01u) != 0u;
      if (adpcm_muted != s_state.adpcm_muted ||
          ((value & 0x20u) &&
           memcmp(s_state.cd_audio_volume_matrix, s_state.next_cd_audio_volume_matrix,
                  sizeof(s_state.cd_audio_volume_matrix)) != 0)) {
        if (cdrom_has_pending_disc_event())
          timing_event_invoke_early(&s_state.drive_event, false);
        spu_generate_pending_samples();
      }
      s_state.adpcm_muted = adpcm_muted;
      if (value & 0x20u)
        memcpy(s_state.cd_audio_volume_matrix, s_state.next_cd_audio_volume_matrix,
               sizeof(s_state.cd_audio_volume_matrix));
      return;
    }

    default:
      ERROR_LOG("Unknown CDROM register write: offset=0x%02X, index=%u, reg=%u, value=0x%02X",
                offset, (unsigned)s_state.status.bits.index, reg, value);
      return;
  }
}

void cdrom_dma_read(u32* words, u32 word_count)
{
  cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];
  const u32 bytes_available =
    (s_state.request_register.bits.BFRD && sb->position < sb->size) ? (sb->size - sb->position) : 0u;
  u8* dst_ptr = (u8*)words;
  u32 bytes_remaining = word_count * (u32)sizeof(u32);

  if (bytes_available > 0) {
    const u32 transfer_size = (bytes_available < bytes_remaining) ? bytes_available : bytes_remaining;
    memcpy(dst_ptr, &sb->data[sb->position], transfer_size);
    sb->position += transfer_size;
    dst_ptr += transfer_size;
    bytes_remaining -= transfer_size;
  }

  if (bytes_remaining > 0) {
    ERROR_LOG("Sector buffer overread by %u bytes", bytes_remaining);
    memset(dst_ptr, 0, bytes_remaining);
  }

  cdrom_check_for_sector_buffer_read_complete();
}

static void cdrom_set_interrupt(cdrom_interrupt_t interrupt)
{
  if (cdrom_trace_enabled()) {
    DEV_LOG("[trace] set_interrupt INT%u (sync) flag=0x%02X enable=0x%02X",
            (unsigned)interrupt, s_state.interrupt_flag_register, s_state.interrupt_enable_register);
  }
  s_state.interrupt_flag_register = (u8)interrupt;
  cdrom_update_interrupt_request();
}

static void cdrom_set_async_interrupt(cdrom_interrupt_t interrupt)
{
  if (cdrom_trace_enabled()) {
    DEV_LOG("[trace] set_async_interrupt INT%u flag=0x%02X pending_async=0x%02X cmd=0x%04X",
            (unsigned)interrupt, s_state.interrupt_flag_register,
            s_state.pending_async_interrupt, (unsigned)s_state.command);
  }
  if (s_state.interrupt_flag_register == (u8)interrupt) {
    DEV_LOG("Not setting async interrupt %u because there is already one unacknowledged", (unsigned)interrupt);
    byte_fifo_clear(&s_state.async_response_fifo);
    return;
  }
  Assert(s_state.pending_async_interrupt == 0);
  s_state.pending_async_interrupt = (u8)interrupt;
  if (!cdrom_has_pending_interrupt()) {
    /* Pending command also blocks INT1; Gokujou Parodius lockup if a
     * command is in flight when the INT1 lands. */
    if (!cdrom_has_pending_command())
      cdrom_queue_deliver_async_interrupt();
    else
      DEBUG_LOG("Delaying async interrupt %u because of pending command", s_state.pending_async_interrupt);
  } else {
    DEBUG_LOG("Delaying async interrupt %u because of pending interrupt %u",
              s_state.pending_async_interrupt, s_state.interrupt_flag_register);
  }
}

static void cdrom_clear_async_interrupt(void)
{
  s_state.pending_async_interrupt = 0;
  timing_event_deactivate(&s_state.async_interrupt_event);
  byte_fifo_clear(&s_state.async_response_fifo);
}

static void cdrom_queue_deliver_async_interrupt(void)
{
  /* Insert a small gap after every INT3 before the next INT1 can fire. */
  DebugAssert(cdrom_has_pending_async_interrupt());

  const u32 diff = (u32)(timing_events_get_global_tick_counter() - s_state.last_interrupt_time);
  if (diff >= CDROM_MINIMUM_INTERRUPT_DELAY) {
    cdrom_deliver_async_interrupt(NULL, 0, 0);
  } else {
    DEV_LOG("Delaying async interrupt %u, %u cycles since last", s_state.pending_async_interrupt, diff);
    timing_event_schedule(&s_state.async_interrupt_event, CDROM_INTERRUPT_DELAY_CYCLES);
  }
}

static void cdrom_deliver_async_interrupt(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks; (void)ticks_late;
  if (cdrom_has_pending_interrupt()) {
    if (cdrom_trace_enabled()) {
      DEV_LOG("[trace] deliver_async_interrupt deferred (flag=0x%02X pending=0x%02X)",
              s_state.interrupt_flag_register, s_state.pending_async_interrupt);
    }
    if (!timing_event_is_active(&s_state.async_interrupt_event))
      timing_event_schedule(&s_state.async_interrupt_event, CDROM_INTERRUPT_DELAY_CYCLES);
  } else {
    timing_event_deactivate(&s_state.async_interrupt_event);
    Assert(s_state.pending_async_interrupt != 0 && !cdrom_has_pending_interrupt());
    if (cdrom_trace_enabled()) {
      DEV_LOG("[trace] deliver_async_interrupt INT%u (rd_buf=%u wr_buf=%u)",
              s_state.pending_async_interrupt,
              s_state.current_read_sector_buffer, s_state.current_write_sector_buffer);
    }
    DEBUG_LOG("Delivering async interrupt %u", s_state.pending_async_interrupt);

    /* The HC05 sub-CPU latches the host-visible read pointer here to mimic
     * the next-sector handoff. */
    if (s_state.pending_async_interrupt == (u8)CDROM_INT_DATA_READY)
      s_state.current_read_sector_buffer = s_state.current_write_sector_buffer;

    byte_fifo_clear(&s_state.response_fifo);
    byte_fifo_push_from_other(&s_state.response_fifo, &s_state.async_response_fifo);
    s_state.interrupt_flag_register = s_state.pending_async_interrupt;
    s_state.pending_async_interrupt = 0;
    cdrom_update_interrupt_request();
    cdrom_update_status_register();
    cdrom_update_command_event();
  }
}

static void cdrom_send_ack_and_stat(void)
{
  byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
  cdrom_set_interrupt(CDROM_INT_ACK);
}

static void cdrom_send_error_response(u8 stat_bits, u8 reason)
{
  byte_fifo_push(&s_state.response_fifo, (u8)(s_state.secondary_status.raw | stat_bits));
  byte_fifo_push(&s_state.response_fifo, reason);
  cdrom_set_interrupt(CDROM_INT_ERROR);
}

static void cdrom_send_async_error_response(u8 stat_bits, u8 reason)
{
  byte_fifo_push(&s_state.async_response_fifo, (u8)(s_state.secondary_status.raw | stat_bits));
  byte_fifo_push(&s_state.async_response_fifo, reason);
  cdrom_set_async_interrupt(CDROM_INT_ERROR);
}

static void cdrom_update_status_register(void)
{
  s_state.status.bits.ADPBUSY = 0;
  s_state.status.bits.PRMEMPTY = byte_fifo_is_empty(&s_state.param_fifo) ? 1u : 0u;
  s_state.status.bits.PRMWRDY  = byte_fifo_is_full(&s_state.param_fifo) ? 0u : 1u;
  s_state.status.bits.RSLRRDY  = byte_fifo_is_empty(&s_state.response_fifo) ? 0u : 1u;
  s_state.status.bits.DRQSTS   = s_state.request_register.bits.BFRD;
  s_state.status.bits.BUSYSTS  = cdrom_has_pending_command() ? 1u : 0u;

  dma_set_request(DMA_CHANNEL_CDROM, s_state.status.bits.DRQSTS != 0u);
}

static void cdrom_update_interrupt_request(void)
{
  interrupt_controller_set_line_state(
    INTERRUPT_CONTROLLER_IRQ_CDROM,
    (s_state.interrupt_flag_register & s_state.interrupt_enable_register) != 0);
}

static bool cdrom_has_pending_disc_event(void)
{
  return timing_event_is_active(&s_state.drive_event) &&
         timing_event_get_ticks_until_next_execution(&s_state.drive_event) <= 0;
}

static bool cdrom_can_use_read_speedup(void)
{
  /* Only use read speedup in 2X mode and when not playing/filtering XA. */
  if (s_state.mode.bits.cdda || s_state.mode.bits.xa_enable || !s_state.mode.bits.double_speed)
    return false;
  if (g_settings.mdec_disable_cdrom_speedup && mdec_is_active && mdec_is_active())
    return false;
  return true;
}

void cdrom_disable_read_speedup(void)
{
  if (s_state.drive_state != CDROM_DRV_READING || !cdrom_can_use_read_speedup())
    return;

  /* MDEC frame start: don't continue at speedup rate or output FIFOs overrun. */
  const tick_count_t expected_ticks =
    (tick_count_t)CDROM_MASTER_CLOCK / CDROM_DOUBLE_SPEED_SECTORS_PER_SECOND;
  const tick_count_t ticks_since = timing_event_get_ticks_since_last_execution(&s_state.drive_event);
  const tick_count_t ticks_until = timing_event_get_ticks_until_next_execution(&s_state.drive_event);
  const tick_count_t sector_ticks = ticks_since + ticks_until;
  if (sector_ticks >= expected_ticks)
    return;

  timing_event_schedule(&s_state.drive_event, expected_ticks - ticks_since);
}

static tick_count_t cdrom_get_ack_delay_for_command(cdrom_command_t cmd)
{
  if (cmd == CDROM_CMD_INIT) return 80000;
  /* Disc in drive: longer ack delay (the HC05 has discy work to do). */
  return cdrom_can_read_media() ? 25000 : 15000;
}

static tick_count_t cdrom_get_ticks_for_spin_up(void) { return CDROM_MASTER_CLOCK; /* 1s */ }

static tick_count_t cdrom_get_ticks_for_id_read(void)
{
  tick_count_t ticks = CDROM_ID_READ_TICKS;
  if (s_state.drive_state == CDROM_DRV_SPINNING_UP)
    ticks += timing_event_get_ticks_until_next_execution(&s_state.drive_event);
  return ticks;
}

static tick_count_t cdrom_get_ticks_for_read(void)
{
  const tick_count_t tps = CDROM_MASTER_CLOCK;
  if (g_settings.cdrom_read_speedup > 1 && cdrom_can_use_read_speedup())
    return tps / (CDROM_DOUBLE_SPEED_SECTORS_PER_SECOND * g_settings.cdrom_read_speedup);
  return s_state.mode.bits.double_speed
           ? (tps / CDROM_DOUBLE_SPEED_SECTORS_PER_SECOND)
           : (tps / CDROM_SINGLE_SPEED_SECTORS_PER_SECOND);
}

static u32 cdrom_get_sectors_per_track(cd_image_lba_t lba)
{
  /* Per-minute sectors-per-track table from real-mech measurements (rama).
   * Beyond minute 71 the mech buggy-clamps to the 71-minute value. */
  static u8 spt_table[80];
  static bool init = false;
  if (!init) {
    for (u32 mm = 0; mm < 80; mm++) {
      if (mm == 0)        spt_table[mm] = 8;
      else if (mm <= 4)   spt_table[mm] = 9;
      else if (mm <= 7)   spt_table[mm] = 10;
      else if (mm <= 11)  spt_table[mm] = 11;
      else if (mm <= 16)  spt_table[mm] = 12;
      else if (mm <= 23)  spt_table[mm] = 13;
      else if (mm <= 27)  spt_table[mm] = 14;
      else if (mm <= 32)  spt_table[mm] = 15;
      else if (mm <= 39)  spt_table[mm] = 16;
      else if (mm <= 44)  spt_table[mm] = 17;
      else if (mm <= 52)  spt_table[mm] = 18;
      else if (mm <= 60)  spt_table[mm] = 19;
      else if (mm <= 67)  spt_table[mm] = 20;
      else if (mm <= 74)  spt_table[mm] = 21;
      else                spt_table[mm] = 22;
    }
    init = true;
  }
  const u32 mm = lba / CD_IMAGE_FRAMES_PER_MINUTE;
  return spt_table[(mm < 80u) ? mm : 79u];
}

static tick_count_t cdrom_get_ticks_for_seek(cd_image_lba_t new_lba, bool ignore_speed_change)
{
  if (g_settings.cdrom_seek_speedup == 0)
    return cdrom_scale_ticks_to_overclock((tick_count_t)g_settings.cdrom_max_seek_speedup_cycles);

  u32 ticks = 0;

  if (cdrom_is_seeking())
    cdrom_update_subq_position_while_seeking();
  else
    cdrom_update_subq_position(false);

  const cd_image_lba_t current_lba =
    cdrom_is_motor_on() ? (cdrom_is_seeking() ? s_state.seek_end_lba : s_state.current_subq_lba) : 0;
  const cd_image_lba_t lba_diff =
    (new_lba > current_lba) ? (new_lba - current_lba) : (current_lba - new_lba);

  if (!cdrom_is_motor_on()) {
     ticks += (s_state.drive_state == CDROM_DRV_SPINNING_UP)
               ? (u32)timing_event_get_ticks_until_next_execution(&s_state.drive_event) 
               : (u32)cdrom_get_ticks_for_spin_up();
    if (s_state.drive_state == CDROM_DRV_SHELL_OPENING || s_state.drive_state == CDROM_DRV_SPINNING_UP)
      cdrom_clear_drive_state();
  }

  const tick_count_t ticks_per_sector =
    s_state.mode.bits.double_speed ? (CDROM_MASTER_CLOCK / 150) : (CDROM_MASTER_CLOCK / 75);
  const cd_image_lba_t sectors_per_track = cdrom_get_sectors_per_track(current_lba);
  const cd_image_lba_t tjump_position = (current_lba >= sectors_per_track) ? (current_lba - sectors_per_track) : 0;

  if (current_lba < new_lba && lba_diff <= sectors_per_track) {
    /* Forward, small distance: just wait for sector to come up. */
    cd_image_lba_t d = (lba_diff < 2u) ? 2u : lba_diff;
    ticks += (u32)ticks_per_sector * d;
  } else if (current_lba >= new_lba && tjump_position <= new_lba) {
    /* 1-track jump back. */
    cd_image_lba_t d = (new_lba - tjump_position);
    if (d < 1u) d = 1u;
    ticks += (u32)ticks_per_sector * d;
  } else if (lba_diff < 7200u) {
    /* Not sled.  Switch point varies across the disc; logarithmic curve. */
    float lba_frac = (float)current_lba / (float)CD_IMAGE_FRAMES_PER_MINUTE;
    if (lba_frac < 1.0f)  lba_frac = 1.0f;
    if (lba_frac > 72.0f) lba_frac = 72.0f;
    const u32 switch_point = (u32)(330.0f + (-63.1333f * logf(lba_frac)));
    const float seconds = (lba_diff < switch_point) ? 0.05f : 0.1f;
    ticks += (u32)(seconds * (float)CDROM_MASTER_CLOCK);
  } else {
    /* Sled: 200ms..900ms, fixed + linear + log. */
    const float SLED_FIXED_COST = 0.05f;
    const float SLED_VARIABLE_COST = 0.9f - SLED_FIXED_COST;
    const float LOG_WEIGHT = 0.4f;
    const float MAX_SLED_LBA = 72.0f * (float)CD_IMAGE_FRAMES_PER_MINUTE;
    const float seconds =
      SLED_FIXED_COST +
      (((SLED_VARIABLE_COST * (logf((float)lba_diff) / logf(MAX_SLED_LBA)))) * LOG_WEIGHT) + 
      ((SLED_VARIABLE_COST * ((float)lba_diff / MAX_SLED_LBA)) * (1.0f - LOG_WEIGHT));
    ticks += (u32)(seconds * (float)CDROM_MASTER_CLOCK);
  }

  /* Some games (RE, Dino Crisis) deadlock if we return identical seek
   * times.  Add 0.5..1ms randomness to mimic real hardware. */
  const u32 lo = CDROM_MASTER_CLOCK / 2000u;
  const u32 hi = CDROM_MASTER_CLOCK / 1000u;
  ticks += lo + (u32)xorshift128pp_next_range(&s_state.prng, hi - lo);

  if (g_settings.cdrom_seek_speedup > 1) {
    u32 spd = ticks / g_settings.cdrom_seek_speedup;
    if ((tick_count_t)spd < CDROM_MIN_SEEK_TICKS) spd = (u32)CDROM_MIN_SEEK_TICKS;
    ticks = spd;
  }

  if (s_state.drive_state == CDROM_DRV_CHANGING_SPEED_OR_TOC_READ && !ignore_speed_change) {
    const tick_count_t remaining = timing_event_get_ticks_until_next_execution(&s_state.drive_event);
    ticks += (u32)remaining;
  }

  return cdrom_scale_ticks_to_overclock((tick_count_t)ticks);
}

static tick_count_t cdrom_get_ticks_for_pause(void)
{
  if (!cdrom_is_reading_or_playing()) return 27000;

  if (g_settings.cdrom_read_speedup == 0 && cdrom_can_use_read_speedup()) {
     u32 m = g_settings.cdrom_max_read_speedup_cycles > g_settings.cdrom_max_seek_speedup_cycles
              ? g_settings.cdrom_max_read_speedup_cycles 
              : g_settings.cdrom_max_seek_speedup_cycles;
    return cdrom_scale_ticks_to_overclock((tick_count_t)m);
  }

  const u32 sectors_per_track = cdrom_get_sectors_per_track(s_state.current_lba);
  const tick_count_t ticks_per_read = cdrom_get_ticks_for_read();

  /* Jump backwards one track, then time to reach target.  Subtract 2 in
   * data mode because we hold based on subq, not data. */
  const tick_count_t ticks_to_reach_target =
    ((tick_count_t)(sectors_per_track - (cdrom_is_reading() ? 2u : 0u)) * ticks_per_read) -
    timing_event_get_ticks_since_last_execution(&s_state.drive_event);

  const tick_count_t min_ticks = s_state.mode.bits.double_speed ? 1000000 : 2000000;
  return ticks_to_reach_target > min_ticks ? ticks_to_reach_target : min_ticks;
}

static tick_count_t cdrom_get_ticks_for_stop(bool motor_was_on)
{
  return cdrom_scale_ticks_to_overclock(
    motor_was_on ? (s_state.mode.bits.double_speed ? 25000000 : 13000000) : 7000);
}

static tick_count_t cdrom_get_ticks_for_speed_change(void)
{
  /* 0.6s 1->2 / 0.7s 2->1 in real-mech timings. */
  static const u32 ticks_single_to_double = (u32)(0.6 * (double)CDROM_MASTER_CLOCK);
  static const u32 ticks_double_to_single = (u32)(0.7 * (double)CDROM_MASTER_CLOCK);
  return cdrom_scale_ticks_to_overclock(
    (tick_count_t)(s_state.mode.bits.double_speed ? ticks_single_to_double : ticks_double_to_single));
}

static tick_count_t cdrom_get_ticks_for_toc_read(void)
{
  if (!cdrom_has_media()) return 0;
  return CDROM_MASTER_CLOCK / 2;
}

static cd_image_lba_t cdrom_get_next_sector_to_be_read(void)
{
  if (!cdrom_is_reading_or_playing() && !cdrom_is_seeking())
    return s_state.current_lba;
  cdrom_async_reader_wait_for_read_to_complete(&s_reader);
  return cdrom_async_reader_get_last_read_sector(&s_reader);
}

/* Ringbuffer of last CDROM commands issued by CPU. Dumped on SIGINT. */
typedef struct {
  u64 tick;
  u32 lba_at_call;
  u8  cmd;
  u8  drive_state;
  u8  nparams;
  u8  params[16];
  u8  pending_int;
  u8  flag_reg;
} cdrom_dbg_cmd_entry_t;

#define CDROM_DBG_CMD_LOG_SIZE 64u
static cdrom_dbg_cmd_entry_t s_dbg_cmd_log[CDROM_DBG_CMD_LOG_SIZE];
static u32 s_dbg_cmd_log_pos;
static u32 s_dbg_cmd_log_count;

/* Per-byte param-FIFO write trace storage (forward-declared above
 * cdrom_write_register so the case-1 path can fill it).  See decl block. */
cdrom_dbg_param_trace_t cdrom_dbg_param_trace[CDROM_DBG_PARAM_TRACE_CAP];
u32 cdrom_dbg_param_trace_pos;
u32 cdrom_dbg_param_trace_count;

static void cdrom_dbg_dump_param_trace(void)
{
  fprintf(stderr, "[cdrom-param-trace] last %u byte(s) pushed (oldest first):\n",
          cdrom_dbg_param_trace_count);
  const u32 n = cdrom_dbg_param_trace_count;
  const u32 start = (n < CDROM_DBG_PARAM_TRACE_CAP) ? 0u
                    : (cdrom_dbg_param_trace_pos % CDROM_DBG_PARAM_TRACE_CAP);
  for (u32 i = 0u; i < n; ++i) {
    const cdrom_dbg_param_trace_t* e =
      &cdrom_dbg_param_trace[(start + i) % CDROM_DBG_PARAM_TRACE_CAP];
    /* Disassemble the writer instruction so we can see which reg sourced
     * the bad byte.  Best-effort; skip on read failure. */
    u32 instr = 0u;
    char dis[96];
    dis[0] = '\0';
    if (cpu_safe_read_memory_word(e->src_pc, &instr)) {
      small_string_t s;
      small_string_init_stack(&s, dis, sizeof(dis));
      cpu_disassemble(&s, instr, e->src_pc);
    }
    fprintf(stderr,
            "  [%2u] tick=%llu value=0x%02X src_pc=0x%08X  inst=0x%08X  %s\n",
            i, (unsigned long long)e->tick, e->value, e->src_pc, instr,
            dis[0] ? dis : "<unreadable>");
  }
  /* Dump current GPRs; the bad bytes were stored shortly before this
   * SetLoc command ran, and the regs may still hold the source pointer +
   * loop counter.  Cheap; only fires on Invalid-seek rejection. */
  fprintf(stderr, "[cdrom-param-trace] GPRs at SetLoc rejection:\n");
  for (int i = 0; i < 32; ++i) {
    fprintf(stderr, "  r%-2d=%08x%s", i, g_cpu_state.regs.r[i],
            ((i % 8) == 7) ? "\n" : "");
  }
  /* Disasm 16 instructions surrounding the LAST trace entry's writer_pc
   * so we can see the loop body around the offending sb v0,0(v1). */
  if (n > 0u) {
    const u32 last_idx = (cdrom_dbg_param_trace_pos + CDROM_DBG_PARAM_TRACE_CAP - 1u)
                         % CDROM_DBG_PARAM_TRACE_CAP;
    const u32 base = cdrom_dbg_param_trace[last_idx].src_pc;
    const u32 disasm_start = base >= 0x40u ? base - 0x40u : 0u;
    fprintf(stderr,
            "[cdrom-param-trace] disasm 16 inst around last writer_pc=0x%08X "
            "(start=0x%08X):\n", base, disasm_start);
    for (u32 i = 0u; i < 32u; ++i) {
      const u32 pc = disasm_start + i * 4u;
      u32 word = 0u;
      char buf[96]; buf[0] = '\0';
      if (cpu_safe_read_memory_word(pc, &word)) {
        small_string_t s;
        small_string_init_stack(&s, buf, sizeof(buf));
        cpu_disassemble(&s, word, pc);
      }
      fprintf(stderr, "  %s%08X: %08X  %s\n",
              (pc == base) ? "->" : "  ", pc, word, buf[0] ? buf : "<unreadable>");
    }
  }
  /* Disasm the LBA->MSF *caller* function body around
   * 0x8002FA80..0x8002FB30 (per the watch-tap dump the writes happen
   * with RA=0x8002FAC8, so the JAL is at 0x8002FAC4 and the surrounding
   * code there sets up $a0 = LBA before the call). */
  {
    fprintf(stderr,
            "[cdrom-param-trace] LBA->MSF CALLER disasm (0x8002FA80..0x8002FB30):\n");
    for (u32 pc = 0x8002FA80u; pc <= 0x8002FB30u; pc += 4u) {
      u32 word = 0u; char buf[96]; buf[0] = '\0';
      if (cpu_safe_read_memory_word(pc, &word)) {
        small_string_t s; small_string_init_stack(&s, buf, sizeof(buf));
        cpu_disassemble(&s, word, pc);
      }
      fprintf(stderr, "    %s%08X: %08X  %s\n",
              (pc == 0x8002FAC4u) ? "->" : "  ", pc, word,
              buf[0] ? buf : "<unreadable>");
    }
  }
  /* Disasm the LBA->MSF function body at 0x80043A24-0x80043B30 (this is
   * where the recomp historically passed a corrupted $a0 to the
   * converter).  Plus its caller around RA-32..RA+8. */
  {
    fprintf(stderr,
            "[cdrom-param-trace] LBA->MSF function disasm (0x80043A24..0x80043B30):\n");
    for (u32 pc = 0x80043A24u; pc <= 0x80043B30u; pc += 4u) {
      u32 word = 0u; char buf[96]; buf[0] = '\0';
      if (cpu_safe_read_memory_word(pc, &word)) {
        small_string_t s; small_string_init_stack(&s, buf, sizeof(buf));
        cpu_disassemble(&s, word, pc);
      }
      fprintf(stderr, "    %08X: %08X  %s\n",
              pc, word, buf[0] ? buf : "<unreadable>");
    }
    const u32 ra = g_cpu_state.regs.r[31];
    const u32 cs = (ra > 0x40u) ? (ra - 0x40u) : 0u;
    fprintf(stderr,
            "[cdrom-param-trace] caller disasm around RA=0x%08X (start=0x%08X, 24 inst):\n",
            ra, cs);
    for (u32 i = 0u; i < 24u; ++i) {
      const u32 pc = cs + i * 4u;
      u32 word = 0u; char buf[96]; buf[0] = '\0';
      if (cpu_safe_read_memory_word(pc, &word)) {
        small_string_t s; small_string_init_stack(&s, buf, sizeof(buf));
        cpu_disassemble(&s, word, pc);
      }
      fprintf(stderr, "    %s%08X: %08X  %s\n",
              (pc == ra) ? "->" : "  ", pc, word, buf[0] ? buf : "<unreadable>");
    }
  }
  /* Dump pointer-looking RAM around RA-1 word and a few common register
   * "base pointer" candidates so we can see what source data fed v0. */
  fprintf(stderr, "[cdrom-param-trace] RAM dumps for likely source/dest pointers:\n");
  static const int probe_regs[] = {5, 6, 17, 18, 20, 21, 22, 31};
  for (size_t pi = 0; pi < sizeof(probe_regs) / sizeof(probe_regs[0]); ++pi) {
    const u32 addr = g_cpu_state.regs.r[probe_regs[pi]];
    /* Only probe addresses that look like RAM/BIOS virtual addresses. */
    if ((addr & 0xE0000000u) != 0x80000000u && (addr & 0xE0000000u) != 0xA0000000u
        && (addr & 0xFE000000u) != 0xBE000000u)
      continue;
    fprintf(stderr, "  r%-2d=%08X ->", probe_regs[pi], addr);
    for (u32 off = 0u; off < 16u; ++off) {
      u32 word = 0u;
      if (cpu_safe_read_memory_word((addr + off) & ~3u, &word)) {
        const u8 b = (u8)((word >> ((((addr + off) & 3u) * 8u))) & 0xFFu);
        fprintf(stderr, " %02X", b);
      } else {
        fprintf(stderr, " ??");
      }
    }
    fputc('\n', stderr);
  }
  fflush(stderr);
}

static void cdrom_begin_command(cdrom_command_t command)
{
  /* Capture pre-state for ringbuffer. */
  {
    cdrom_dbg_cmd_entry_t* e = &s_dbg_cmd_log[s_dbg_cmd_log_pos];
    e->tick        = (u64)timing_events_get_global_tick_counter();
    e->cmd         = (u8)command;
    e->drive_state = (u8)s_state.drive_state;
    e->lba_at_call = s_state.current_lba;
    e->pending_int = (u8)s_state.pending_async_interrupt;
    e->flag_reg    = s_state.interrupt_flag_register;
    u32 n = byte_fifo_size(&s_state.param_fifo);
    if (n > 16u) n = 16u;
    e->nparams = (u8)n;
    for (u32 i = 0; i < n; i++) e->params[i] = byte_fifo_peek(&s_state.param_fifo, i);
    s_dbg_cmd_log_pos = (s_dbg_cmd_log_pos + 1u) % CDROM_DBG_CMD_LOG_SIZE;
    if (s_dbg_cmd_log_count < CDROM_DBG_CMD_LOG_SIZE) s_dbg_cmd_log_count++;
  }

  tick_count_t ack_delay = cdrom_get_ack_delay_for_command(command);

  if (cdrom_has_pending_command()) {
    /* If the new command's parameter count is smaller than the existing one,
     * drop it; otherwise override.  Voice Idol Collection - Pool Bar Story
     * relies on this. */
    if (s_command_info[(u8)s_state.command].min_parameters >
        s_command_info[(u8)command].min_parameters) {
      WARNING_LOG("Ignoring command 0x%02X and emptying FIFO as 0x%02X is still pending",
                  (unsigned)command, (unsigned)s_state.command);
      byte_fifo_clear(&s_state.param_fifo);
      return;
    }
    WARNING_LOG("Cancelling pending command 0x%02X for new command 0x%02X",
                (unsigned)s_state.command, (unsigned)command);

    if (timing_event_is_active(&s_state.command_event)) {
      const tick_count_t elapsed =
        timing_event_get_interval(&s_state.command_event) -
        timing_event_get_ticks_until_next_execution(&s_state.command_event);
      tick_count_t adj = ack_delay - elapsed;
      ack_delay = (adj > 1) ? adj : 1;
      timing_event_deactivate(&s_state.command_event);

      /* Pending async interrupt was being held by the command we just
       * cancelled; release it so SF Alpha 3 doesn't deadlock. */
      if (cdrom_has_pending_async_interrupt()) {
        WARNING_LOG("Delivering pending interrupt after command cancellation.");
        cdrom_queue_deliver_async_interrupt();
      }
    }
  }

  s_state.command = command;
  timing_event_set_interval_and_schedule(&s_state.command_event, ack_delay);
  cdrom_update_command_event();
  cdrom_update_status_register();
}

static void cdrom_end_command(void)
{
  byte_fifo_clear(&s_state.param_fifo);
  s_state.command = CDROM_CMD_NONE;
  timing_event_deactivate(&s_state.command_event);
  cdrom_update_status_register();
}

static void cdrom_update_command_event(void)
{
  if (!cdrom_has_pending_command() || cdrom_has_pending_interrupt() || cdrom_has_pending_async_interrupt()) {
    timing_event_deactivate(&s_state.command_event);
    return;
  }
  if (cdrom_has_pending_command())
    timing_event_activate(&s_state.command_event);
}

static void cdrom_clear_command_second_response(void)
{
  if (s_state.command_second_response != CDROM_CMD_NONE) {
    DEV_LOG("Cancelling pending command 0x%04X second response",
            (unsigned)s_state.command_second_response);
  }
  timing_event_deactivate(&s_state.command_second_response_event);
  s_state.command_second_response = CDROM_CMD_NONE;
}

static void cdrom_queue_command_second_response(cdrom_command_t cmd, tick_count_t ticks)
{
  cdrom_clear_command_second_response();
  s_state.command_second_response = cmd;
  timing_event_schedule(&s_state.command_second_response_event, ticks);
}

static void cdrom_execute_command(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks;
  const cdrom_command_info_t* ci = &s_command_info[(u8)s_state.command];
  if (byte_fifo_size(&s_state.param_fifo) < ci->min_parameters ||
      byte_fifo_size(&s_state.param_fifo) > ci->max_parameters) {
    WARNING_LOG("Incorrect parameters for command 0x%02X (%s), expecting %u-%u got %u",
                (unsigned)s_state.command, ci->name ? ci->name : "Unknown",
                (unsigned)ci->min_parameters, (unsigned)ci->max_parameters, 
                byte_fifo_size(&s_state.param_fifo));
    cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INCORRECT_NUMBER_OF_PARAMETERS);
    cdrom_end_command();
    return;
  }

  if (!byte_fifo_is_empty(&s_state.response_fifo)) {
    DEBUG_LOG("Response FIFO not empty on command begin");
    byte_fifo_clear(&s_state.response_fifo);
  }

  timing_event_deactivate(&s_state.command_event);

  switch (s_state.command) {
    case CDROM_CMD_GETSTAT:
      cdrom_send_ack_and_stat();
      if (cdrom_can_read_media())
        s_state.secondary_status.bits.shell_open = 0;
      cdrom_end_command();
      return;

    case CDROM_CMD_TEST: {
      const u8 sub = byte_fifo_pop(&s_state.param_fifo);
      cdrom_execute_test_command(sub);
      return;
    }

    case CDROM_CMD_GETID:
      cdrom_clear_command_second_response();
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        cdrom_queue_command_second_response(CDROM_CMD_GETID, cdrom_get_ticks_for_id_read());
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_READTOC:
      cdrom_clear_command_second_response();
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        cdrom_set_hold_position(0, 0);
        cdrom_queue_command_second_response(CDROM_CMD_READTOC, cdrom_get_ticks_for_toc_read());
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_SETFILTER: {
      const u8 file    = byte_fifo_peek(&s_state.param_fifo, 0);
      const u8 channel = byte_fifo_peek(&s_state.param_fifo, 1);
      s_state.xa_filter_file_number    = file;
      s_state.xa_filter_channel_number = channel;
      s_state.xa_current_set = false;
      cdrom_send_ack_and_stat();
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_SETMODE: {
      const u8 mode = byte_fifo_peek(&s_state.param_fifo, 0);
      const bool speed_change = ((mode & 0x80u) != (s_state.mode.raw & 0x80u));
      s_state.mode.raw = mode;
      cdrom_send_ack_and_stat();
      cdrom_end_command();

      if (speed_change) {
        if (s_state.drive_state == CDROM_DRV_CHANGING_SPEED_OR_TOC_READ) {
          if (timing_event_get_ticks_until_next_execution(&s_state.drive_event) >=
              (cdrom_get_ticks_for_speed_change() / 4))
            cdrom_clear_drive_state();
        } else if (s_state.drive_state != CDROM_DRV_SEEKING_IMPLICIT &&
                   s_state.drive_state != CDROM_DRV_SHELL_OPENING) {
          const tick_count_t change_ticks = cdrom_get_ticks_for_speed_change();
          if (s_state.drive_state != CDROM_DRV_IDLE) {
            timing_event_delay(&s_state.drive_event, change_ticks);
            if (cdrom_is_reading_or_playing()) {
              WARNING_LOG("Speed change while reading/playing, reads will be temporarily delayed.");
              timing_event_set_interval(&s_state.drive_event, cdrom_get_ticks_for_read());
            }
          } else {
            s_state.drive_state = CDROM_DRV_CHANGING_SPEED_OR_TOC_READ;
            timing_event_schedule(&s_state.drive_event, change_ticks);
          }
        }
      }
      return;
    }

    case CDROM_CMD_SETLOC: {
      const u8 mm = byte_fifo_peek(&s_state.param_fifo, 0);
      const u8 ss = byte_fifo_peek(&s_state.param_fifo, 1);
      const u8 ff = byte_fifo_peek(&s_state.param_fifo, 2);

      /* Validate BCD ranges: MM 00..99, SS 00..59, FF 00..74. */
      if (((mm & 0x0Fu) > 0x09u) || (mm > 0x99u) ||
          ((ss & 0x0Fu) > 0x09u) || (ss >= 0x60u) ||
          ((ff & 0x0Fu) > 0x09u) || (ff >= 0x75u)) {
        ERROR_LOG("Invalid seek to %02X:%02X:%02X", mm, ss, ff);
        /* Dump per-byte param-write trace so we can disassemble the
         * sb that wrote the offending non-BCD nibble. */
        cdrom_dbg_dump_param_trace();
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_ARGUMENT);
      } else {
        cdrom_send_ack_and_stat();
        s_state.setloc_position.minute = PackedBCDToBinary(mm);
        s_state.setloc_position.second = PackedBCDToBinary(ss);
        s_state.setloc_position.frame  = PackedBCDToBinary(ff);
        s_state.setloc_pending = true;
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_SEEKL:
    case CDROM_CMD_SEEKP: {
      const bool logical = (s_state.command == CDROM_CMD_SEEKL);
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        cdrom_begin_seeking(logical, false, false);
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_READT: {
      const u8 session = byte_fifo_peek(&s_state.param_fifo, 0);
      if (!cdrom_can_read_media() || s_state.drive_state == CDROM_DRV_READING ||
          s_state.drive_state == CDROM_DRV_PLAYING) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else if (session == 0) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_ARGUMENT);
      } else {
        cdrom_clear_command_second_response();
        cdrom_send_ack_and_stat();
        s_state.async_command_parameter = session;
        s_state.drive_state = CDROM_DRV_CHANGING_SESSION;
        timing_event_schedule(&s_state.drive_event, cdrom_get_ticks_for_toc_read());
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_READN:
    case CDROM_CMD_READS:
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else if ((cdrom_is_media_audio_cd() || !cdrom_does_media_region_match_console()) &&
                 !s_state.mode.bits.cdda) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_COMMAND);
      } else {
        cdrom_send_ack_and_stat();
        const cd_image_lba_t setloc_lba =
          cd_image_position_to_lba(s_state.setloc_position);
        if ((!s_state.setloc_pending || setloc_lba == cdrom_get_next_sector_to_be_read()) &&
            (s_state.drive_state == CDROM_DRV_READING ||
             (cdrom_is_seeking() && s_state.read_after_seek))) {
          s_state.setloc_pending = false;
        } else {
          cdrom_begin_reading(0, false);
        }
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_PLAY: {
      const u8 track = byte_fifo_is_empty(&s_state.param_fifo)
                         ? 0u
                         : PackedBCDToBinary(byte_fifo_peek(&s_state.param_fifo, 0));
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        const cd_image_lba_t setloc_lba = cd_image_position_to_lba(s_state.setloc_position);
        if (track == 0 &&
            (!s_state.setloc_pending || setloc_lba == cdrom_get_next_sector_to_be_read()) &&
            (s_state.drive_state == CDROM_DRV_PLAYING || 
             (cdrom_is_seeking() && s_state.play_after_seek))) {
          s_state.fast_forward_rate = 0;
          s_state.setloc_pending = false;
        } else {
          cdrom_begin_playing(track, 0, false);
        }
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_FORWARD:
      if (s_state.drive_state != CDROM_DRV_PLAYING || !cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        if (s_state.fast_forward_rate < 0) s_state.fast_forward_rate = 0;
        s_state.fast_forward_rate += (s8)CDROM_FAST_FORWARD_RATE_STEP;
        if (s_state.fast_forward_rate > (s8)CDROM_MAX_FAST_FORWARD_RATE)
          s_state.fast_forward_rate = (s8)CDROM_MAX_FAST_FORWARD_RATE;
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_BACKWARD:
      if (s_state.drive_state != CDROM_DRV_PLAYING || !cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        if (s_state.fast_forward_rate > 0) s_state.fast_forward_rate = 0;
        s_state.fast_forward_rate -= (s8)CDROM_FAST_FORWARD_RATE_STEP;
        if (s_state.fast_forward_rate < -(s8)CDROM_MAX_FAST_FORWARD_RATE)
          s_state.fast_forward_rate = -(s8)CDROM_MAX_FAST_FORWARD_RATE;
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_PAUSE: {
      const tick_count_t pause_time = cdrom_get_ticks_for_pause();
      if (cdrom_is_reading() && cd_image_subq_is_data(&s_state.last_subq)) {
        const u32 spt = cdrom_get_sectors_per_track(s_state.current_lba);
        cdrom_set_hold_position(s_state.current_lba,
                                (spt <= s_state.current_lba) ? (s_state.current_lba - spt) : 0);
      }

      cdrom_clear_command_second_response();
      cdrom_send_ack_and_stat();

      /* Hardware: pause is rejected if the drive just started a read/seek
       * and hasn't processed the first sector yet. */
      if (s_state.drive_state == CDROM_DRV_SEEKING_LOGICAL ||
          s_state.drive_state == CDROM_DRV_SEEKING_PHYSICAL ||
          ((s_state.drive_state == CDROM_DRV_READING || s_state.drive_state == CDROM_DRV_PLAYING) && 
           s_state.secondary_status.bits.seeking)) {
        WARNING_LOG("CDROM Pause command while seeking - sending error response");
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
        cdrom_end_command();
        return;
      }

      cdrom_clear_async_interrupt();

      s_state.drive_state = CDROM_DRV_IDLE;
      timing_event_deactivate(&s_state.drive_event);
      cdrom_secondary_clear_active_bits();

      cdrom_reset_audio_decoder();

      cdrom_queue_command_second_response(CDROM_CMD_PAUSE, pause_time);
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_STOP: {
      const tick_count_t stop_time = cdrom_get_ticks_for_stop(cdrom_is_motor_on());
      cdrom_clear_async_interrupt();
      cdrom_clear_command_second_response();
      cdrom_send_ack_and_stat();
      cdrom_stop_motor();
      cdrom_queue_command_second_response(CDROM_CMD_STOP, stop_time);
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_INIT: {
      if (s_state.command_second_response == CDROM_CMD_INIT) {
        cdrom_end_command();
        return;
      }
      cdrom_send_ack_and_stat();
      const tick_count_t reset_ticks = cdrom_soft_reset(ticks_late);
      cdrom_queue_command_second_response(CDROM_CMD_INIT, reset_ticks);
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_MOTORON:
      if (cdrom_is_motor_on()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INCORRECT_NUMBER_OF_PARAMETERS);
      } else if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_send_ack_and_stat();
        if (s_state.command_second_response == CDROM_CMD_MOTORON) {
          cdrom_end_command();
          return;
        }
        s_state.secondary_status.bits.motor_on = 1; /* Armored Core relies on this. */
        cdrom_start_motor();
        cdrom_queue_command_second_response(CDROM_CMD_MOTORON, CDROM_MOTOR_ON_RESPONSE_TICKS);
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_MUTE:
      s_state.muted = true;
      cdrom_send_ack_and_stat();
      cdrom_end_command();
      return;

    case CDROM_CMD_DEMUTE:
      s_state.muted = false;
      cdrom_send_ack_and_stat();
      cdrom_end_command();
      return;

    case CDROM_CMD_GETLOCL:
      if (!s_state.last_sector_header_valid) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        cdrom_update_subq_position(true);
        byte_fifo_push_range(&s_state.response_fifo,
                             (const u8*)&s_state.last_sector_header,
                             (u32)sizeof(s_state.last_sector_header));
        byte_fifo_push_range(&s_state.response_fifo,
                             (const u8*)&s_state.last_sector_subheader,
                             (u32)sizeof(s_state.last_sector_subheader));
        cdrom_set_interrupt(CDROM_INT_ACK);
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_GETLOCP:
      if (!cdrom_can_read_media()) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      } else {
        if (cdrom_is_seeking()) cdrom_update_subq_position_while_seeking();
        else                    cdrom_update_subq_position(false);
        cdrom_ensure_last_subq_valid();

        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.track_number_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.index_number_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.relative_minute_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.relative_second_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.relative_frame_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.absolute_minute_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.absolute_second_bcd);
        byte_fifo_push(&s_state.response_fifo, s_state.last_subq.absolute_frame_bcd);
        cdrom_set_interrupt(CDROM_INT_ACK);
      }
      cdrom_end_command();
      return;

    case CDROM_CMD_GETTN: {
      const cd_image_t* media = cdrom_async_reader_get_media_const(&s_reader);
      if (cdrom_can_read_media() && media) {
        byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
        byte_fifo_push(&s_state.response_fifo, BinaryToBCD((u8)cd_image_get_first_track_number(media)));
        byte_fifo_push(&s_state.response_fifo, BinaryToBCD((u8)cd_image_get_last_track_number(media)));
        cdrom_set_interrupt(CDROM_INT_ACK);
      } else {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_GETTD: {
      Assert(byte_fifo_size(&s_state.param_fifo) >= 1);
      cd_image_t* media = cdrom_async_reader_get_media(&s_reader);
      if (!cdrom_can_read_media() || !media) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_NOT_READY);
        cdrom_end_command();
        return;
      }
      const u8 track_bcd = byte_fifo_peek(&s_state.param_fifo, 0);
      if (!IsValidPackedBCD(track_bcd)) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_ARGUMENT);
        cdrom_end_command();
        return;
      }
      const u8 track = PackedBCDToBinary(track_bcd);
      if (track > cd_image_get_track_count(media)) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_ARGUMENT);
      } else {
        cd_image_position_t pos;
        if (track == 0)
          pos = cd_image_position_from_lba(cd_image_get_lba_count(media));
        else
          pos = cd_image_get_track_start_msf_position(media, track);

        byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
        byte_fifo_push(&s_state.response_fifo, BinaryToBCD(pos.minute));
        byte_fifo_push(&s_state.response_fifo, BinaryToBCD(pos.second));
        cdrom_set_interrupt(CDROM_INT_ACK);
      }
      cdrom_end_command();
      return;
    }

    case CDROM_CMD_GETMODE:
      byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
      byte_fifo_push(&s_state.response_fifo, s_state.mode.raw);
      byte_fifo_push(&s_state.response_fifo, 0);
      byte_fifo_push(&s_state.response_fifo, s_state.xa_filter_file_number);
      byte_fifo_push(&s_state.response_fifo, s_state.xa_filter_channel_number);
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;

    case CDROM_CMD_SYNC:
      ERROR_LOG("Invalid sync command");
      cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_COMMAND);
      cdrom_end_command();
      return;

    case CDROM_CMD_VIDEOCD:
      ERROR_LOG("Invalid VideoCD command");
      cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_COMMAND);
      /* Per nocash: VideoCD doesn't clear the param FIFO. */
      s_state.command = CDROM_CMD_NONE;
      timing_event_deactivate(&s_state.command_event);
      cdrom_update_status_register();
      return;

    default:
      ERROR_LOG("Unknown CDROM command 0x%04X with %u parameters",
                (unsigned)s_state.command, byte_fifo_size(&s_state.param_fifo));
      cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_COMMAND);
      cdrom_end_command();
      return;
  }
}

static void cdrom_execute_test_command(u8 subcommand)
{
  switch (subcommand) {
    case 0x04: /* Reset SCEx counters */
      s_state.secondary_status.bits.motor_on = 1;
      byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;

    case 0x05: /* Read SCEx counters */
      byte_fifo_push(&s_state.response_fifo, s_state.secondary_status.raw);
      byte_fifo_push(&s_state.response_fifo, 0);
      byte_fifo_push(&s_state.response_fifo, 0);
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;

    case 0x20: { /* Get CDROM BIOS Date/Version */
      static const u8 version_table[][4] = {
        {0x94, 0x09, 0x19, 0xC0}, /* PSX (PU-7)               */
        {0x94, 0x11, 0x18, 0xC0}, /* PSX (PU-7)               */
        {0x95, 0x05, 0x16, 0xC1}, /* PSX (EARLY-PU-8)         */
        {0x95, 0x07, 0x24, 0xC1}, /* PSX (LATE-PU-8)          */
        {0x95, 0x07, 0x24, 0xD1}, /* PSX (LATE-PU-8, debug)   */
        {0x96, 0x08, 0x15, 0xC2}, /* PSX (PU-16, Video CD)    */
        {0x96, 0x08, 0x18, 0xC1}, /* PSX (LATE-PU-8, yaroze)  */
        {0x96, 0x09, 0x12, 0xC2}, /* PSX (PU-18) (japan)      */
        {0x97, 0x01, 0x10, 0xC2}, /* PSX (PU-18) (us/eur)     */
        {0x97, 0x08, 0x14, 0xC2}, /* PSX (PU-20)              */
        {0x98, 0x06, 0x10, 0xC3}, /* PSX (PU-22)              */
        {0x99, 0x02, 0x01, 0xC3}, /* PSX/PSone (PU-23, PM-41) */
        {0xA1, 0x03, 0x06, 0xC3}, /* PSone/late (PM-41(2))    */
      };
      u8 idx = (u8)g_settings.cdrom_mechacon_version;
      if (idx >= (sizeof(version_table) / sizeof(version_table[0])))
        idx = 0;
      byte_fifo_push_range(&s_state.response_fifo, version_table[idx], 4);
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;
    }

    case 0x22: { /* Get CDROM region ID string */
      switch (cdrom_get_console_region()) {
        case CONSOLE_REGION_NTSC_J: {
          static const u8 r[] = {'f', 'o', 'r', ' ', 'J', 'a', 'p', 'a', 'n'};
          byte_fifo_push_range(&s_state.response_fifo, r, sizeof(r));
          break;
        }
        case CONSOLE_REGION_PAL: {
          static const u8 r[] = {'f', 'o', 'r', ' ', 'E', 'u', 'r', 'o', 'p', 'e'};
          byte_fifo_push_range(&s_state.response_fifo, r, sizeof(r));
          break;
        }
        case CONSOLE_REGION_NTSC_U:
        default: {
          static const u8 r[] = {'f', 'o', 'r', ' ', 'U', '/', 'C'};
          byte_fifo_push_range(&s_state.response_fifo, r, sizeof(r));
          break;
        }
      }
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;
    }

    case 0x60: { /* Read memory address (returns 0). */
      if (byte_fifo_size(&s_state.param_fifo) < 2) {
        cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INCORRECT_NUMBER_OF_PARAMETERS);
        cdrom_end_command();
        return;
      }
      byte_fifo_push(&s_state.response_fifo, 0x00);
      cdrom_set_interrupt(CDROM_INT_ACK);
      cdrom_end_command();
      return;
    }

    default:
      ERROR_LOG("Unknown test command 0x%02X, %u parameters", subcommand,
                byte_fifo_size(&s_state.param_fifo));
      cdrom_send_error_response(CDROM_STAT_ERROR, CDROM_ERR_INVALID_COMMAND);
      cdrom_end_command();
      return;
  }
}

static void cdrom_execute_command_second_response(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks; (void)ticks_late;
  switch (s_state.command_second_response) {
    case CDROM_CMD_GETID:
      cdrom_do_id_read();
      break;

    case CDROM_CMD_INIT:
      /* OpenBIOS spams Init; ensure the completion gets through. */
      if (cdrom_has_pending_command()) {
        WARNING_LOG("Cancelling pending command 0x%04X due to init completion.",
                    (unsigned)s_state.command);
        cdrom_end_command();
      }
      cdrom_do_stat_second_response();
      break;

    case CDROM_CMD_READTOC:
    case CDROM_CMD_PAUSE:
    case CDROM_CMD_MOTORON:
      cdrom_do_stat_second_response();
      break;

    case CDROM_CMD_STOP:
      cdrom_do_stat_second_response();
      /* cdrom_auto_disc_change requires a system module hook; not ported. */
      break;

    default:
      break;
  }

  s_state.command_second_response = CDROM_CMD_NONE;
  timing_event_deactivate(&s_state.command_second_response_event);
}

static void cdrom_execute_drive(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks;
  switch (s_state.drive_state) {
    case CDROM_DRV_SHELL_OPENING:
      cdrom_do_shell_open_complete(ticks_late);
      break;
    case CDROM_DRV_SEEKING_PHYSICAL:
    case CDROM_DRV_SEEKING_LOGICAL:
      cdrom_do_seek_complete(ticks_late);
      break;
    case CDROM_DRV_SEEKING_IMPLICIT:
      cdrom_complete_seek();
      break;
    case CDROM_DRV_READING:
    case CDROM_DRV_PLAYING:
      cdrom_do_sector_read();
      break;
    case CDROM_DRV_CHANGING_SESSION:
      cdrom_do_change_session_complete();
      break;
    case CDROM_DRV_SPINNING_UP:
      cdrom_do_spin_up_complete();
      break;
    case CDROM_DRV_CHANGING_SPEED_OR_TOC_READ:
      cdrom_do_speed_change_or_implicit_toc_read_complete();
      break;

    /* Save-state-compat for old states (kept for parity). */
    case CDROM_DRV_UNUSED_READING_ID:
      cdrom_clear_drive_state();
      cdrom_do_id_read();
      break;
    case CDROM_DRV_UNUSED_RESETTING:
    case CDROM_DRV_UNUSED_READING_TOC:
      cdrom_clear_drive_state();
      cdrom_do_stat_second_response();
      break;
    case CDROM_DRV_UNUSED_PAUSING:
      cdrom_clear_drive_state();
      cdrom_secondary_clear_active_bits();
      cdrom_do_stat_second_response();
      break;
    case CDROM_DRV_UNUSED_STOPPING:
      cdrom_clear_drive_state();
      cdrom_stop_motor();
      cdrom_do_stat_second_response();
      break;

    case CDROM_DRV_IDLE:
    default:
      break;
  }
}

static void cdrom_clear_drive_state(void)
{
  s_state.drive_state = CDROM_DRV_IDLE;
  timing_event_deactivate(&s_state.drive_event);
}

static void cdrom_begin_reading(tick_count_t ticks_late, bool after_seek)
{
  if (!after_seek && s_state.setloc_pending) {
    cdrom_begin_seeking(true, true, false);
    return;
  }

  if (cdrom_is_seeking()) {
    DEV_LOG("Read while seeking, scheduling read after seek finishes");
    if (s_state.drive_state == CDROM_DRV_SEEKING_IMPLICIT)
      s_state.drive_state = CDROM_DRV_SEEKING_LOGICAL;
    s_state.read_after_seek = true;
    s_state.play_after_seek = false;
    return;
  }

  DEBUG_LOG("Starting reading @ LBA %u", s_state.current_lba);

  const tick_count_t ticks = cdrom_get_ticks_for_read();
  const tick_count_t first_sector_ticks =
    ticks + (after_seek ? 0 : cdrom_get_ticks_for_seek(s_state.current_lba, false)) - ticks_late;

  cdrom_clear_command_second_response();
  cdrom_clear_async_interrupt();
  cdrom_clear_sector_buffers();
  cdrom_reset_audio_decoder();

  if (!after_seek)
    cdrom_secondary_set_seeking();

  s_state.drive_state = CDROM_DRV_READING;
  timing_event_set_interval(&s_state.drive_event, ticks);
  timing_event_schedule(&s_state.drive_event, first_sector_ticks);

  s_state.requested_lba = s_state.current_lba;
  s_state.seek_start_lba = 0;
  s_state.seek_end_lba = 0;
  cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
}

static void cdrom_begin_playing(u8 track, tick_count_t ticks_late, bool after_seek)
{
  DEBUG_LOG("Starting playing CDDA track %u", track);
  s_state.play_track_number_bcd = track;
  s_state.fast_forward_rate = 0;

  cd_image_t* media = cdrom_async_reader_get_media(&s_reader);

  /* Track 0 means "current position".  Otherwise resolve to track start. */
  if (track != 0 && media) {
    if (track > cd_image_get_track_count(media))
      track = (u8)cd_image_get_track_number(media);
    s_state.setloc_position = cd_image_get_track_start_msf_position(media, track);
    s_state.setloc_pending = true;
  }

  if (s_state.setloc_pending) {
    cdrom_begin_seeking(false, false, true);
    return;
  }

  const tick_count_t ticks = cdrom_get_ticks_for_read();
  const tick_count_t first_sector_ticks =
    ticks + (after_seek ? 0 : cdrom_get_ticks_for_seek(s_state.current_lba, true)) - ticks_late;

  cdrom_clear_command_second_response();
  cdrom_clear_async_interrupt();
  cdrom_clear_sector_buffers();
  cdrom_reset_audio_decoder();

  s_state.cdda_report_start_delay = CDROM_CDDA_REPORT_START_DELAY;
  s_state.last_cdda_report_frame_nibble = 0xFF;

  s_state.drive_state = CDROM_DRV_PLAYING;
  timing_event_set_interval(&s_state.drive_event, ticks);
  timing_event_schedule(&s_state.drive_event, first_sector_ticks);

  s_state.requested_lba = s_state.current_lba;
  cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
}

static void cdrom_begin_seeking(bool logical, bool read_after_seek_, bool play_after_seek_)
{
  if (!s_state.setloc_pending)
    WARNING_LOG("Seeking without setloc set");

  s_state.read_after_seek = read_after_seek_;
  s_state.play_after_seek = play_after_seek_;
  s_state.setloc_pending = false;

  const cd_image_lba_t seek_lba = cd_image_position_to_lba(s_state.setloc_position);
  tick_count_t seek_time;

  /* Repeated SeekL to the same target completes nearly instantly when we
   * still hold the SubQ at target-2 (Resident Evil 3 needs this). */
  if (logical && !read_after_seek_ &&
      s_state.current_subq_lba == (seek_lba - CDROM_SUBQ_SECTOR_SKEW) &&
      s_state.seek_end_lba == seek_lba &&
      (timing_events_get_global_tick_counter() - s_state.subq_lba_update_tick) < 
        (global_ticks_t)cdrom_get_ticks_for_read()) {
    seek_time = CDROM_MIN_SEEK_TICKS;
  } else {
    seek_time = cdrom_get_ticks_for_seek(seek_lba, play_after_seek_);
  }

  cdrom_clear_command_second_response();
  cdrom_clear_async_interrupt();
  cdrom_clear_sector_buffers();
  cdrom_reset_audio_decoder();

  cdrom_secondary_set_seeking();
  s_state.last_sector_header_valid = false;

  s_state.drive_state = logical ? CDROM_DRV_SEEKING_LOGICAL : CDROM_DRV_SEEKING_PHYSICAL;
  timing_event_set_interval_and_schedule(&s_state.drive_event, seek_time);

  s_state.seek_start_lba = s_state.current_lba;
  s_state.seek_end_lba   = seek_lba;
  s_state.requested_lba  = seek_lba;
  cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
}

static void cdrom_update_subq_position_while_seeking(void)
{
  DebugAssert(cdrom_is_seeking());

  const tick_count_t until = timing_event_get_ticks_until_next_execution(&s_state.drive_event);
  const tick_count_t interval = timing_event_get_interval(&s_state.drive_event);
  float frac = 1.0f - ((float)until / (float)interval);
  if (frac < 0.0f) frac = 0.0f;
  if (frac > 1.0f) frac = 1.0f;

  cd_image_lba_t current_lba;
  if (s_state.seek_end_lba > s_state.seek_start_lba) {
    cd_image_lba_t step = (cd_image_lba_t)((float)(s_state.seek_end_lba - s_state.seek_start_lba) * frac);
    if (step < 1) step = 1;
    current_lba = s_state.seek_start_lba + step;
  } else if (s_state.seek_end_lba < s_state.seek_start_lba) {
    cd_image_lba_t step = (cd_image_lba_t)((float)(s_state.seek_start_lba - s_state.seek_end_lba) * frac);
    if (step < 1) step = 1;
    current_lba = s_state.seek_start_lba - step;
  } else {
    return;
  }

  s_state.last_subq_needs_update = (s_state.current_subq_lba != current_lba);
  s_state.current_subq_lba = current_lba;
  s_state.subq_lba_update_tick = timing_events_get_global_tick_counter();
  s_state.subq_lba_update_carry = 0;
}

static void cdrom_update_subq_position(bool update_logical)
{
  const global_ticks_t now = timing_events_get_global_tick_counter();
  if (cdrom_is_seeking() || cdrom_is_reading_or_playing() || !cdrom_is_motor_on()) {
    if ((s_state.secondary_status.raw & (CDROM_STAT_READING | CDROM_STAT_PLAYING_CDDA | CDROM_STAT_MOTOR_ON)) ==
        CDROM_STAT_MOTOR_ON && s_state.current_lba != s_state.current_subq_lba) {
      cdrom_set_hold_position(s_state.current_lba, s_state.current_lba);
    }
    return;
  }

  const u32 ticks_per_read = (u32)cdrom_get_ticks_for_read();
  const u32 diff = (u32)((now - s_state.subq_lba_update_tick) + s_state.subq_lba_update_carry);
  const u32 sector_diff = diff / ticks_per_read;
  const u32 carry = diff % ticks_per_read;
  if (sector_diff > 0) {
    const cd_image_lba_t hold_offset = s_state.last_sector_header_valid ? 2u : 0u;
    const cd_image_lba_t spt = cdrom_get_sectors_per_track(s_state.current_lba);
    const cd_image_lba_t hold_position = s_state.current_lba + hold_offset;
    const cd_image_lba_t tjump = (hold_position >= spt) ? (hold_position - spt) : 0u;
    const cd_image_lba_t old_off = s_state.current_subq_lba - tjump;
    const cd_image_lba_t new_off = (old_off + sector_diff) % spt;
    const cd_image_lba_t new_subq_lba = tjump + new_off;
    if (s_state.current_subq_lba != new_subq_lba) {
      s_state.current_subq_lba = new_subq_lba;
      s_state.last_subq_needs_update = true;
      s_state.subq_lba_update_tick = now;
      s_state.subq_lba_update_carry = carry;

      if (update_logical) {
        cd_image_subq_t real_subq;
        memset(&real_subq, 0, sizeof(real_subq));
        cdrom_async_sector_buffer_t raw_sector;
        if (!cdrom_async_reader_read_sector_uncached(&s_reader, new_subq_lba, &real_subq, &raw_sector)) {
          ERROR_LOG("Failed to read subq for sector %u", new_subq_lba);
        } else {
          s_state.last_subq_needs_update = false;
          const cd_image_subq_t* subq = cdrom_get_sector_subq(new_subq_lba, &real_subq);
          if (cd_image_subq_is_crc_valid(subq))
            s_state.last_subq = *subq;
          cdrom_process_data_sector_header(raw_sector);
        }
      }
    }
  }
}

static void cdrom_set_hold_position(cd_image_lba_t lba, cd_image_lba_t subq_lba)
{
  s_state.last_subq_needs_update |= (s_state.current_subq_lba != subq_lba);
  s_state.current_lba = lba;
  s_state.current_subq_lba = subq_lba;
  s_state.subq_lba_update_tick = timing_events_get_global_tick_counter();
  s_state.subq_lba_update_carry = 0;
}

static void cdrom_ensure_last_subq_valid(void)
{
  if (!s_state.last_subq_needs_update) return;
  s_state.last_subq_needs_update = false;

  cd_image_subq_t real_subq;
  memset(&real_subq, 0, sizeof(real_subq));
  if (!cdrom_async_reader_read_sector_uncached(&s_reader, s_state.current_subq_lba, &real_subq, NULL))
    ERROR_LOG("Failed to read subq for sector %u", s_state.current_subq_lba);

  const cd_image_subq_t* subq = cdrom_get_sector_subq(s_state.current_subq_lba, &real_subq);
  if (cd_image_subq_is_crc_valid(subq))
    s_state.last_subq = *subq;
}

static void cdrom_do_shell_open_complete(tick_count_t ticks_late)
{
  (void)ticks_late;
  cdrom_clear_drive_state();
  if (cdrom_can_read_media())
    cdrom_start_motor();
}

static bool cdrom_complete_seek(void)
{
  const bool logical = (s_state.drive_state == CDROM_DRV_SEEKING_LOGICAL);
  cdrom_clear_drive_state();

  bool seek_okay = cdrom_async_reader_wait_for_read_to_complete(&s_reader);

  s_state.current_subq_lba = cdrom_async_reader_get_last_read_sector(&s_reader);
  s_state.last_subq_needs_update = false;
  s_state.subq_lba_update_tick = timing_events_get_global_tick_counter();
  s_state.subq_lba_update_carry = 0;

  if (seek_okay) {
    const cd_image_subq_t* real_subq = cdrom_async_reader_get_sector_subq(&s_reader);
    const cd_image_subq_t* subq = cdrom_get_sector_subq(
      cdrom_async_reader_get_last_read_sector(&s_reader), real_subq);
    s_state.current_lba = cdrom_async_reader_get_last_read_sector(&s_reader);

    if (cd_image_subq_is_crc_valid(subq)) {
      s_state.last_subq = *subq;
      s_state.last_subq_needs_update = false;

      u8 seek_mm, seek_ss, seek_ff;
      cd_image_position_to_bcd(cd_image_position_from_lba(cdrom_async_reader_get_last_read_sector(&s_reader)),
                               &seek_mm, &seek_ss, &seek_ff);
      seek_okay = (subq->absolute_minute_bcd == seek_mm &&
                   subq->absolute_second_bcd == seek_ss &&
                   subq->absolute_frame_bcd  == seek_ff);
      if (seek_okay) {
        if (cd_image_subq_is_data(subq)) {
          if (logical) {
            const u8* sb_data = (const u8*)cdrom_async_reader_get_sector_buffer(&s_reader);
            cdrom_process_data_sector_header(sb_data);
            seek_okay = (s_state.last_sector_header.minute == seek_mm &&
                         s_state.last_sector_header.second == seek_ss &&
                         s_state.last_sector_header.frame  == seek_ff);

            if (seek_okay && !s_state.play_after_seek && !s_state.read_after_seek) {
              /* Hold SubQ -2 from data header so subsequent seeks read
               * a seek when it sees the data header for target-2. */
              s_state.current_subq_lba = (s_state.current_lba >= CDROM_SUBQ_SECTOR_SKEW)
                                           ? (s_state.current_lba - CDROM_SUBQ_SECTOR_SKEW) 
                                           : 0;
              s_state.last_subq_needs_update = true;
            }
          }
        } else {
          if (logical) {
            WARNING_LOG("Logical seek to non-data sector [%02x:%02x:%02x]", seek_mm, seek_ss, seek_ff);
            /* Wizard's Harmony seeks to audio and expects success;
             * Vib-ribbon starts a read at audio and expects fail.  Gate on
             * cdda mode for the read-after-seek case. */
            if (s_state.read_after_seek)
              seek_okay = (s_state.mode.bits.cdda != 0);
          }
        }

        if (subq->track_number_bcd == CD_IMAGE_LEAD_OUT_TRACK_NUMBER) {
          WARNING_LOG("Invalid seek to lead-out area");
          seek_okay = false;
        }
      }
    }
  }
  return seek_okay;
}

static void cdrom_do_seek_complete(tick_count_t ticks_late)
{
  const bool logical = (s_state.drive_state == CDROM_DRV_SEEKING_LOGICAL);
  const bool seek_okay = cdrom_complete_seek();
  if (seek_okay) {
    if (s_state.read_after_seek) {
      cdrom_begin_reading(ticks_late, true);
    } else if (s_state.play_after_seek) {
      cdrom_begin_playing(0, ticks_late, true);
    } else {
      cdrom_secondary_clear_active_bits();
      byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
      cdrom_set_async_interrupt(CDROM_INT_COMPLETE);
    }
  } else {
    WARNING_LOG("%s seek failed", logical ? "Logical" : "Physical");
    cdrom_secondary_clear_active_bits();
    cdrom_send_async_error_response(CDROM_STAT_SEEK_ERROR, 0x04);
    s_state.last_sector_header_valid = false;
  }

  s_state.setloc_pending = false;
  s_state.read_after_seek = false;
  s_state.play_after_seek = false;
  cdrom_update_status_register();
}

static void cdrom_do_stat_second_response(void)
{
  if (!cdrom_can_read_media()) {
    cdrom_send_async_error_response(CDROM_STAT_ERROR, 0x08);
    return;
  }
  byte_fifo_clear(&s_state.async_response_fifo);
  byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
  cdrom_set_async_interrupt(CDROM_INT_COMPLETE);
}

static void cdrom_do_change_session_complete(void)
{
  cdrom_clear_drive_state();
  cdrom_secondary_clear_active_bits();
  s_state.secondary_status.bits.motor_on = 1;

  byte_fifo_clear(&s_state.async_response_fifo);
  if (s_state.async_command_parameter == 0x01) {
    byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
    cdrom_set_async_interrupt(CDROM_INT_COMPLETE);
  } else {
    /* Multi-session not emulated. */
    cdrom_send_async_error_response(CDROM_STAT_SEEK_ERROR, 0x40);
  }
}

static void cdrom_do_spin_up_complete(void)
{
  s_state.drive_state = CDROM_DRV_IDLE;
  timing_event_deactivate(&s_state.drive_event);
  cdrom_secondary_clear_active_bits();
  s_state.secondary_status.bits.motor_on = 1;
}

static void cdrom_do_speed_change_or_implicit_toc_read_complete(void)
{
  s_state.drive_state = CDROM_DRV_IDLE;
  timing_event_deactivate(&s_state.drive_event);
}

static void cdrom_do_id_read(void)
{
  cdrom_secondary_clear_active_bits();
  s_state.secondary_status.bits.motor_on = cdrom_can_read_media();

  u8 stat_byte = s_state.secondary_status.raw;
  u8 flags_byte = 0;
  if (!cdrom_can_read_media()) {
    stat_byte |= CDROM_STAT_ID_ERROR;
    flags_byte |= (1u << 6); /* Disc Missing */
  } else {
    if (cdrom_is_media_audio_cd()) {
      stat_byte |= CDROM_STAT_ID_ERROR;
      flags_byte |= (1u << 7) | (1u << 4); /* Unlicensed + Audio CD */
    } else if (!cdrom_is_media_ps1_disc() || !cdrom_does_media_region_match_console()) {
      stat_byte |= CDROM_STAT_ID_ERROR;
      flags_byte |= (1u << 7); /* Unlicensed */
    }
  }

  byte_fifo_clear(&s_state.async_response_fifo);
  byte_fifo_push(&s_state.async_response_fifo, stat_byte);
  byte_fifo_push(&s_state.async_response_fifo, flags_byte);
  byte_fifo_push(&s_state.async_response_fifo, 0x20); /* disc type from TOC; TODO */
  byte_fifo_push(&s_state.async_response_fifo, 0x00); /* session info */

  static const u8 region_strings[DISC_REGION_COUNT][4] = {
    {'S', 'C', 'E', 'I'}, /* NTSC_J */
    {'S', 'C', 'E', 'A'}, /* NTSC_U */
    {'S', 'C', 'E', 'E'}, /* PAL    */
    {0, 0, 0, 0},          /* Other  */
    {0, 0, 0, 0},          /* NonPS1 */
  };
  const u8 r = (u8)s_state.disc_region;
  byte_fifo_push_range(&s_state.async_response_fifo,
                       region_strings[r < DISC_REGION_COUNT ? r : DISC_REGION_NON_PS1], 4);

  cdrom_set_async_interrupt(flags_byte != 0 ? CDROM_INT_ERROR : CDROM_INT_COMPLETE);
}

static void cdrom_stop_reading_with_data_end(void)
{
  cdrom_clear_async_interrupt();
  byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
  cdrom_set_async_interrupt(CDROM_INT_DATA_END);
  cdrom_secondary_clear_active_bits();
  cdrom_clear_drive_state();
}

static void cdrom_stop_reading_with_error(u8 reason)
{
  cdrom_clear_async_interrupt();
  cdrom_send_async_error_response(CDROM_STAT_ERROR, reason);
  cdrom_secondary_clear_active_bits();
  cdrom_clear_drive_state();
}

static void cdrom_start_motor(void)
{
  if (s_state.drive_state == CDROM_DRV_SPINNING_UP) return;
  s_state.drive_state = CDROM_DRV_SPINNING_UP;
  timing_event_schedule(&s_state.drive_event, cdrom_get_ticks_for_spin_up());
}

static void cdrom_stop_motor(void)
{
  cdrom_secondary_clear_active_bits();
  s_state.secondary_status.bits.motor_on = 0;
  cdrom_clear_drive_state();
  cdrom_set_hold_position(0, 0);
  s_state.last_sector_header_valid = false;
}

static void cdrom_do_sector_read(void)
{
  if (!cdrom_async_reader_wait_for_read_to_complete(&s_reader)) {
    /* Disc Read Error; no OSD layer, just log and abort. */
    fputs("CDROM: Disc Read Error.  The game will probably crash now.\n", stderr);
    cdrom_stop_reading_with_error(CDROM_ERR_NOT_READY);
    return;
  }

  s_state.current_lba = cdrom_async_reader_get_last_read_sector(&s_reader);
  s_state.current_subq_lba = s_state.current_lba;
  s_state.last_subq_needs_update = false;
  s_state.subq_lba_update_tick = timing_events_get_global_tick_counter();
  s_state.subq_lba_update_carry = 0;

  cdrom_secondary_set_reading_bits(s_state.drive_state == CDROM_DRV_PLAYING);

  const cd_image_subq_t* real_subq = cdrom_async_reader_get_sector_subq(&s_reader);
  const cd_image_subq_t* subq = cdrom_get_sector_subq(s_state.current_lba, real_subq);
  const bool subq_valid = cd_image_subq_is_crc_valid(subq);
  if (subq_valid) {
    s_state.last_subq = *subq;
    if (g_settings.cdrom_subq_skew) {
      /* Captain Commando: synthetic SubQ +2 so its DMA-race detection */
      cd_image_t* media = cdrom_async_reader_get_media(&s_reader);
      if (media)
        cd_image_generate_subq(media, &s_state.last_subq, s_state.current_lba + CDROM_SUBQ_SECTOR_SKEW);
    }
  }

  if (subq->track_number_bcd == CD_IMAGE_LEAD_OUT_TRACK_NUMBER) {
    cdrom_stop_reading_with_data_end();
    cdrom_stop_motor();
    return;
  }

  const bool is_data_sector = cd_image_subq_is_data(subq);
  const u8* raw_sector = (const u8*)cdrom_async_reader_get_sector_buffer(&s_reader);

  if (is_data_sector) {
    cdrom_process_data_sector_header(raw_sector);
  } else if (s_state.mode.bits.auto_pause) {
    if (s_state.cdda_auto_pause_pending) {
      s_state.cdda_auto_pause_pending = false;
      cdrom_stop_reading_with_data_end();
      return;
    }

    /* Defer arming auto-pause until we've seen a couple of sectors with
     * the same track number; Pitball's pregap-less menu music tricks us
     * otherwise. */
    if (s_state.play_track_number_bcd == 0) {
      s_state.play_track_number_bcd = subq->track_number_bcd;
    } else if (s_state.play_track_number_bcd != subq->track_number_bcd) {
      s_state.cdda_auto_pause_pending = true;
    }
  }

  u32 next_sector = s_state.current_lba + 1u;
  if (is_data_sector && s_state.drive_state == CDROM_DRV_READING) {
    cdrom_process_data_sector(raw_sector, subq);
  } else if (!is_data_sector &&
             (s_state.drive_state == CDROM_DRV_PLAYING ||
              (s_state.drive_state == CDROM_DRV_READING && s_state.mode.bits.cdda))) {
    cdrom_process_cdda_sector(raw_sector, subq, subq_valid);
    if (s_state.fast_forward_rate != 0)
      next_sector = (u32)((s32)s_state.current_lba + (s32)s_state.fast_forward_rate);
  } else if (s_state.drive_state != CDROM_DRV_READING && s_state.drive_state != CDROM_DRV_PLAYING) {
    Panic("Not reading or playing");
  } else {
    WARNING_LOG("Skipping sector %u as it doesn't match drive mode", s_state.current_lba);
  }

  s_state.requested_lba = next_sector;
  cdrom_async_reader_queue_read_sector(&s_reader, s_state.requested_lba);
}

static void cdrom_process_data_sector_header(const u8* raw_sector)
{
  if (!raw_sector) return;
  memcpy(&s_state.last_sector_header, &raw_sector[CDROM_SECTOR_SYNC_SIZE],
         sizeof(s_state.last_sector_header));
  memcpy(&s_state.last_sector_subheader,
         &raw_sector[CDROM_SECTOR_SYNC_SIZE + sizeof(s_state.last_sector_header)],
         sizeof(s_state.last_sector_subheader));
  s_state.last_sector_header_valid = true;
}

static void cdrom_process_data_sector(const u8* raw_sector, const cd_image_subq_t* subq)
{
  (void)subq;
  const u32 sb_num = (s_state.current_write_sector_buffer + 1) % CDROM_NUM_SECTOR_BUFFERS;

  if (s_state.mode.bits.xa_enable && s_state.last_sector_header.sector_mode == 2) {
    if (s_state.last_sector_subheader.submode.bits.realtime &&
        s_state.last_sector_subheader.submode.bits.audio) {
      cdrom_process_xa_adpcm_sector(raw_sector, subq);
      /* Audio+realtime sectors aren't delivered to the CPU. */
      return;
    }
  }

  cdrom_sector_buffer_t* sb = &s_state.sector_buffers[sb_num];
  if (sb->position == 0 && sb->size > 0) {
    DEV_LOG("Sector buffer %u was not read, previous sector dropped",
            (unsigned)((s_state.current_write_sector_buffer - 1) % CDROM_NUM_SECTOR_BUFFERS));
  }

  if (s_state.mode.bits.ignore_bit)
    WARNING_LOG("SetMode.4 bit set on read of sector %u", s_state.current_lba);

  if (s_state.mode.bits.read_raw_sector) {
    if (s_state.last_sector_header.sector_mode == 1) {

      memcpy(&sb->data[0], raw_sector + CDROM_SECTOR_SYNC_SIZE, CDROM_MODE1_HEADER_SIZE);
      memset(&sb->data[CDROM_MODE1_HEADER_SIZE], 0,
             CDROM_MODE2_HEADER_SIZE - CDROM_MODE1_HEADER_SIZE);
      memcpy(&sb->data[CDROM_MODE2_HEADER_SIZE],
             raw_sector + CDROM_SECTOR_SYNC_SIZE + CDROM_MODE1_HEADER_SIZE,
             CDROM_DATA_SECTOR_OUTPUT_SIZE);
      sb->size = CDROM_MODE2_HEADER_SIZE + CDROM_DATA_SECTOR_OUTPUT_SIZE;
    } else {
      memcpy(sb->data, raw_sector + CDROM_SECTOR_SYNC_SIZE, CDROM_RAW_SECTOR_OUTPUT_SIZE);
      sb->size = CDROM_RAW_SECTOR_OUTPUT_SIZE;
    }
  } else {
    if (s_state.last_sector_header.sector_mode != 1 && s_state.last_sector_header.sector_mode != 2) {
      WARNING_LOG("Ignoring non-MODE1/MODE2 sector at %u", s_state.current_lba);
      return;
    }
    const u32 offset = (s_state.last_sector_header.sector_mode == 1)
                         ? (CDROM_SECTOR_SYNC_SIZE + CDROM_MODE1_HEADER_SIZE)
                         : (CDROM_SECTOR_SYNC_SIZE + CDROM_MODE2_HEADER_SIZE);
    memcpy(sb->data, raw_sector + offset, CDROM_DATA_SECTOR_OUTPUT_SIZE);
    sb->size = CDROM_DATA_SECTOR_OUTPUT_SIZE;
  }

  sb->position = 0;
  s_state.current_write_sector_buffer = sb_num;

  if (cdrom_has_pending_async_interrupt()) {
    WARNING_LOG("Data interrupt was not delivered");
    cdrom_clear_async_interrupt();
  }

  if (cdrom_has_pending_interrupt()) {
    const u32 missed =
      (s_state.current_write_sector_buffer - s_state.current_read_sector_buffer) % CDROM_NUM_SECTOR_BUFFERS;
    if (missed > 1)
      WARNING_LOG("Interrupt not processed in time, missed %u sectors", missed - 1);
  }

  byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
  cdrom_set_async_interrupt(CDROM_INT_DATA_READY);
}

void cdrom_get_audio_frame(s16* out_left, s16* out_right)
{
  const u32 frame = audio_fifo_is_empty(&s_state.audio_fifo) ? 0u : audio_fifo_pop(&s_state.audio_fifo);
  const s16 left  = (s16)(frame & 0xFFFFu);
  const s16 right = (s16)((frame >> 16) & 0xFFFFu);
  *out_left  = cdrom_saturate_volume(cdrom_apply_volume(left,  s_state.cd_audio_volume_matrix[0][0]) +
                                     cdrom_apply_volume(right, s_state.cd_audio_volume_matrix[1][0]));
  *out_right = cdrom_saturate_volume(cdrom_apply_volume(left,  s_state.cd_audio_volume_matrix[0][1]) +
                                     cdrom_apply_volume(right, s_state.cd_audio_volume_matrix[1][1]));
}

u32 cdrom_dbg_drive_state(void)
{
  return (u32)s_state.drive_state;
}

u32 cdrom_dbg_current_lba(void)
{
  return s_state.current_lba;
}

u32 cdrom_dbg_requested_lba(void)
{
  return s_state.requested_lba;
}

u32 cdrom_dbg_buffer_status(void)
{
  const cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];
  return ((s_state.current_read_sector_buffer & 0x0Fu) << 28) |
         ((s_state.current_write_sector_buffer & 0x0Fu) << 24) |
         ((s_state.request_register.bits.BFRD & 0x01u) << 23) |
         ((sb->position & 0x7FFu) << 11) | 
         (sb->size & 0x7FFu);
}

u32 cdrom_dbg_command_state(void)
{
  return (((u32)s_state.command & 0xFFu) << 24) |
         (((u32)s_state.command_second_response & 0xFFu) << 16) |
         (((u32)s_state.interrupt_flag_register & 0x1Fu) << 11) |
         (((u32)s_state.pending_async_interrupt & 0x1Fu) << 6) |
         (((u32)s_state.response_fifo.count & 0x07u) << 3) | 
         ((u32)s_state.async_response_fifo.count & 0x07u);
}

void cdrom_dbg_dump_recent_commands(FILE* out)
{
  if (!out) out = stderr;
  fprintf(out, "===== last %u CDROM commands (oldest first) =====\n", s_dbg_cmd_log_count);
  const u32 n = s_dbg_cmd_log_count;
  const u32 start = (n < CDROM_DBG_CMD_LOG_SIZE) ? 0u
                    : (s_dbg_cmd_log_pos % CDROM_DBG_CMD_LOG_SIZE);
  for (u32 i = 0; i < n; i++) {
    const cdrom_dbg_cmd_entry_t* e = &s_dbg_cmd_log[(start + i) % CDROM_DBG_CMD_LOG_SIZE];
    const char* nm = s_command_info[e->cmd].name;
    fprintf(out, "  [%2u] tick=%llu cmd=0x%02X (%s) drv=%u lba=%u ifreg=0x%02X pend_int=%u params(%u):",
            i, (unsigned long long)e->tick, e->cmd, nm ? nm : "?",
            e->drive_state, e->lba_at_call, e->flag_reg, e->pending_int, e->nparams);
    for (u32 p = 0; p < e->nparams; p++) fprintf(out, " %02X", e->params[p]);
    fputc('\n', out);
  }
  fprintf(out, "  current: command=0x%02X drv=%u lba=%u req_lba=%u ifreg=0x%02X pend_async=%u\n",
          (u8)s_state.command, (u32)s_state.drive_state,
          s_state.current_lba, s_state.requested_lba, 
          s_state.interrupt_flag_register, (u8)s_state.pending_async_interrupt);
}

static void cdrom_add_audio_frame(s16 left, s16 right)
{
  audio_fifo_push(&s_state.audio_fifo,
                  ((u32)(u16)left) | (((u32)(u16)right) << 16));
}

static s32 cdrom_apply_volume(s16 sample, u8 volume) { return ((s32)sample * (s32)volume) >> 7; }

static s16 cdrom_saturate_volume(s32 v)
{
  if (v < -0x8000) return (s16)-0x8000;
  if (v >  0x7FFF) return (s16) 0x7FFF;
  return (s16)v;
}

static void cdrom_decode_xa_adpcm_chunks(const u8* chunk_ptr, s16* samples, bool is_stereo, bool is_8bit)
{
  static const s8 filter_table_pos[16] = { 0,  60, 115,  98, 0,0,0,0,0,0,0,0,0,0,0,0 };
  static const s8 filter_table_neg[16] = { 0,   0, -52, -55, 0,0,0,0,0,0,0,0,0,0,0,0 };

  const u32 NUM_CHUNKS = 18;
  const u32 CHUNK_SIZE_IN_BYTES = 128;
  const u32 WORDS_PER_BLOCK = 28;
  const u32 NUM_BLOCKS = is_8bit ? 4u : 8u;
  const u32 SAMPLES_PER_CHUNK = 28u * (is_8bit ? 4u : 8u);

  for (u32 i = 0; i < NUM_CHUNKS; i++) {
    const u8* headers_ptr = chunk_ptr + 4;
    const u8* words_ptr   = chunk_ptr + 16;

    for (u32 block = 0; block < NUM_BLOCKS; block++) {
      const u8 hdr = headers_ptr[block];
      u8 shift = (u8)(hdr & 0x0Fu);
      if (shift > 12) shift = 9;
      const u8  filter = (u8)((hdr >> 4) & 0x0Fu);
      const s32 filter_pos = filter_table_pos[filter];
      const s32 filter_neg = filter_table_neg[filter];

       s16* out = is_stereo
                   ? &samples[(block / 2u) * (WORDS_PER_BLOCK * 2u) + (block % 2u)] 
                   : &samples[block * WORDS_PER_BLOCK];
      const u32 out_inc = is_stereo ? 2u : 1u;

      for (u32 word = 0; word < 28u; word++) {
        u32 word_data;
        memcpy(&word_data, &words_ptr[word * sizeof(u32)], sizeof(word_data));

        const u32 nibble = is_8bit ? ((word_data >> (block * 8u)) & 0xFFu)
                                   : ((word_data >> (block * 4u)) & 0x0Fu);
        const s16 sample = (s16)((s16)((u16)(nibble << (is_8bit ? 8u : 12u))) >> shift);

        s32* prev = is_stereo ? &s_state.xa_last_samples[(block & 1u) * 2u]
                              : &s_state.xa_last_samples[0];
        s32 interp = (s32)sample + ((prev[0] * filter_pos) >> 6) + ((prev[1] * filter_neg) >> 6);
        if (interp < -32768) interp = -32768;
        if (interp >  32767) interp =  32767;

        prev[1] = prev[0];
        prev[0] = interp;
        *out = (s16)interp;
        out += out_inc;
      }
    }

    samples += SAMPLES_PER_CHUNK;
    chunk_ptr += CHUNK_SIZE_IN_BYTES;
  }
}

static s16 xa_zigzag_interpolate(const s16* ringbuf, u32 table_index, u32 p)
{
  static const s16 tables[7][29] = {
    {0,      0x0,     0x0,     0x0,    0x0,     -0x0002, 0x000A,  -0x0022, 0x0041, -0x0054,
     0x0034, 0x0009,  -0x010A, 0x0400, -0x0A78, 0x234C,  0x6794,  -0x1780, 0x0BCD, -0x0623,
     0x0350, -0x016D, 0x006B,  0x000A, -0x0010, 0x0011,  -0x0008, 0x0003,  -0x0001}, 
    {0,       0x0,    0x0,     -0x0002, 0x0,    0x0003,  -0x0013, 0x003C,  -0x004B, 0x00A2,
     -0x00E3, 0x0132, -0x0043, -0x0267, 0x0C9D, 0x74BB,  -0x11B4, 0x09B8,  -0x05BF, 0x0372,
     -0x01A8, 0x00A6, -0x001B, 0x0005,  0x0006, -0x0008, 0x0003,  -0x0001, 0x0}, 
    {0,      0x0,     -0x0001, 0x0003,  -0x0002, -0x0005, 0x001F,  -0x004A, 0x00B3, -0x0192,
     0x02B1, -0x039E, 0x04F8,  -0x05A6, 0x7939,  -0x05A6, 0x04F8,  -0x039E, 0x02B1, -0x0192,
     0x00B3, -0x004A, 0x001F,  -0x0005, -0x0002, 0x0003,  -0x0001, 0x0,     0x0}, 
    {0,       -0x0001, 0x0003,  -0x0008, 0x0006, 0x0005,  -0x001B, 0x00A6, -0x01A8, 0x0372,
     -0x05BF, 0x09B8,  -0x11B4, 0x74BB,  0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2,
     -0x004B, 0x003C,  -0x0013, 0x0003,  0x0,    -0x0002, 0x0,     0x0,    0x0}, 
    {-0x0001, 0x0003,  -0x0008, 0x0011,  -0x0010, 0x000A, 0x006B,  -0x016D, 0x0350, -0x0623,
     0x0BCD,  -0x1780, 0x6794,  0x234C,  -0x0A78, 0x0400, -0x010A, 0x0009,  0x0034, -0x0054,
     0x0041,  -0x0022, 0x000A,  -0x0001, 0x0,     0x0001, 0x0,     0x0,     0x0}, 
    {0x0002,  -0x0008, 0x0010,  -0x0023, 0x002B, 0x001A,  -0x00EB, 0x027B,  -0x0548, 0x0AFA,
     -0x16FA, 0x53E0,  0x3C07,  -0x1249, 0x080E, -0x0347, 0x015B,  -0x0044, -0x0017, 0x0046,
     -0x0023, 0x0011,  -0x0005, 0x0,     0x0,    0x0,     0x0,     0x0,     0x0}, 
    {-0x0005, 0x0011,  -0x0023, 0x0046, -0x0017, -0x0044, 0x015B,  -0x0347, 0x080E, -0x1249,
     0x3C07,  0x53E0,  -0x16FA, 0x0AFA, -0x0548, 0x027B,  -0x00EB, 0x001A,  0x002B, -0x0023,
     0x0010,  -0x0008, 0x0002,  0x0,    0x0,     0x0,     0x0,     0x0,     0x0}, 
  };

  const s16* table = tables[table_index];
  s32 sum = 0;
  for (u32 i = 0; i < 29u; i++)
    sum += ((s32)ringbuf[(p - i) & 0x1Fu] * (s32)table[i]) >> 15;
  if (sum < -0x8000) sum = -0x8000;
  if (sum >  0x7FFF) sum =  0x7FFF;
  return (s16)sum;
}

static void cdrom_resample_xa_adpcm(const s16* frames_in, u32 num_frames_in, bool stereo)
{
  s16* left_ringbuf  = s_state.xa_resample_ring_buffer[0];
  s16* right_ringbuf = s_state.xa_resample_ring_buffer[1];
  u32 p = s_state.xa_resample_p;
  u32 sixstep = s_state.xa_resample_sixstep;

  for (u32 in_idx = 0; in_idx < num_frames_in; in_idx++) {
    left_ringbuf[p] = *(frames_in++);
    if (stereo) right_ringbuf[p] = *(frames_in++);
    p = (p + 1u) % 32u;
    sixstep--;

    if (sixstep == 0) {
      sixstep = 6;
      for (u32 j = 0; j < 7u; j++) {
        const s16 left_interp  = xa_zigzag_interpolate(left_ringbuf, j, p);
        const s16 right_interp = stereo ? xa_zigzag_interpolate(right_ringbuf, j, p) : left_interp;
        cdrom_add_audio_frame(left_interp, right_interp);
      }
    }
  }

  s_state.xa_resample_p = (u8)p;
  s_state.xa_resample_sixstep = (u8)sixstep;
}

static s16 xa_18900_interpolate(const s16* ringbuf, u32 table_index, u32 p)
{
  static const s16 tables[7][25] = {
    {0x0,    -0x5,  0x11,   -0x23, 0x46,  -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0,
     -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a,  0x2b,  -0x23, 0x10,   -0x8,  0x2,     0x0},
    {0x0,    -0x2,  0xa,    -0x22, 0x41,   -0x54, 0x34, 0x9,   -0x10a, 0x400, -0xa78, 0x234c, 0x6794,
     -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b,  0xa,  -0x10, 0x11,   -0x8,  0x3,    -0x1},
    {-0x2,    0x0,   0x3,    -0x13, 0x3c,   -0x4b, 0xa2,  -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb,
     -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6,  -0x1b, 0x5,   0x6,   -0x8,  0x3,    -0x1},
    {-0x1,   0x3,   -0x2,   -0x5,  0x1f,   -0x4a, 0xb3,  -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939,
     -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3,  -0x4a, 0x1f,   -0x5,  -0x2,   0x3,   -0x1},
    {-0x1,  0x3,    -0x8,  0x6,   0x5,   -0x1b, 0xa6,  -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb,
     0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2,  -0x4b, 0x3c,   -0x13, 0x3,    0x0,   -0x2},
    {-0x1,   0x3,    -0x8,  0x11,   -0x10, 0xa,  0x6b,  -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794,
     0x234c, -0xa78, 0x400, -0x10a, 0x9,   0x34, -0x54, 0x41,   -0x22, 0xa,    -0x2,  0x0},
    {0x0,    0x2,     -0x8,  0x10,   -0x23, 0x2b,  0x1a,  -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0,
     0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46,  -0x23, 0x11,   -0x5,  0x0},
  };
  const s16* table = tables[table_index];
  s32 sum = 0;
  for (u32 i = 0; i < 25u; i++)
    sum += (s32)ringbuf[(p + 32u - 25u + i) & 0x1Fu] * (s32)table[i];
  sum >>= 15;
  if (sum < -0x8000) sum = -0x8000;
  if (sum >  0x7FFF) sum =  0x7FFF;
  return (s16)sum;
}

static void cdrom_resample_xa_adpcm_18900(const s16* frames_in, u32 num_frames_in, bool stereo)
{
  s16* left_ringbuf  = s_state.xa_resample_ring_buffer[0];
  s16* right_ringbuf = s_state.xa_resample_ring_buffer[1];
  u32 p = s_state.xa_resample_p;
  u32 sixstep = s_state.xa_resample_sixstep;

  for (u32 in_idx = 0; in_idx < num_frames_in;) {
    if (sixstep >= 7u) {
      sixstep -= 7u;
      p = (p + 1u) % 32u;
      left_ringbuf[p] = *(frames_in++);
      if (stereo) right_ringbuf[p] = *(frames_in++);
      in_idx++;
    }
    const s16 left_interp  = xa_18900_interpolate(left_ringbuf, sixstep, p);
    const s16 right_interp = stereo ? xa_18900_interpolate(right_ringbuf, sixstep, p) : left_interp;
    cdrom_add_audio_frame(left_interp, right_interp);
    sixstep += 3u;
  }

  s_state.xa_resample_p = (u8)p;
  s_state.xa_resample_sixstep = (u8)sixstep;
}

static void cdrom_reset_current_xa_file(void)
{
  s_state.xa_current_channel_number = 0;
  s_state.xa_current_file_number = 0;
  s_state.xa_current_set = false;
}

static void cdrom_reset_audio_decoder(void)
{
  s_state.cdda_auto_pause_pending = false;
  cdrom_reset_current_xa_file();
  memset(s_state.xa_last_samples, 0, sizeof(s_state.xa_last_samples));
  memset(s_state.xa_resample_ring_buffer, 0, sizeof(s_state.xa_resample_ring_buffer));
  s_state.xa_resample_p = 0;
  s_state.xa_resample_sixstep = 6;
  audio_fifo_clear(&s_state.audio_fifo);
}

static void cdrom_process_xa_adpcm_sector(const u8* raw_sector, const cd_image_subq_t* subq)
{
  (void)subq;
  if (s_state.mode.bits.xa_filter &&
      (s_state.last_sector_subheader.file_number    != s_state.xa_filter_file_number ||
       s_state.last_sector_subheader.channel_number != s_state.xa_filter_channel_number)) {
    return;
  }

  if (!s_state.xa_current_set) {
    /* Skip junk channel-number-255 sectors (Taxi 2 / Blues Clues). */
    if (s_state.last_sector_subheader.channel_number == 255 &&
        (!s_state.mode.bits.xa_filter || s_state.xa_filter_channel_number != 255)) {
      return;
    }
    s_state.xa_current_file_number    = s_state.last_sector_subheader.file_number;
    s_state.xa_current_channel_number = s_state.last_sector_subheader.channel_number;
    s_state.xa_current_set = true;
  } else if (s_state.last_sector_subheader.file_number    != s_state.xa_current_file_number ||
             s_state.last_sector_subheader.channel_number != s_state.xa_current_channel_number) {
    return;
  }

  if (s_state.last_sector_subheader.submode.bits.eof)
    cdrom_reset_current_xa_file();

  spu_generate_pending_samples();

  /* Audio FIFO catch-up: if we're more than the low-water mark ahead,
   * skip this XA sector to avoid clobbering interpolation history.  Test
   * case: Simple 1500 Series Vol. 92; The Tozan RPG. */
  cdrom_xa_codinginfo_t cinfo = s_state.last_sector_subheader.codinginfo;
  const u32 num_frames = xa_codinginfo_samples_per_sector(cinfo) >>
                         (xa_codinginfo_is_stereo(cinfo) ? 1u : 0u);
  if (audio_fifo_size(&s_state.audio_fifo) > CDROM_AUDIO_FIFO_LOW_WATERMARK)
    return;

  s16 sample_buffer[CDROM_XA_ADPCM_SAMPLES_PER_SECTOR_4BIT];
  const u8* xa_block_start = raw_sector + CD_IMAGE_SECTOR_SYNC_SIZE +
                              sizeof(cd_image_sector_header_t) + sizeof(cdrom_xa_subheader_t) * 2u;
  s_state.xa_current_codinginfo.raw = cinfo.raw;

  /* A1: first 256 bytes of raw sector (= sync + header + 2x subheader + chunk0
   * head). Tag packs codinginfo (low 8) | file (16-23) | channel (24-31). */
  FMV_DUMP("A1", raw_sector, 256u,
           ((u64)cinfo.raw) |
           ((u64)s_state.last_sector_subheader.file_number << 16) |
           ((u64)s_state.last_sector_subheader.channel_number << 24));

  cdrom_decode_xa_adpcm_chunks(xa_block_start, sample_buffer,
                               xa_codinginfo_is_stereo(cinfo),
                               xa_codinginfo_is_8bit(cinfo));

  /* A2: decoded PCM samples (full sector_4bit buffer = 4032 s16 = 8064B).
   * Tag = num_frames. */
  FMV_DUMP("A2", sample_buffer, sizeof(sample_buffer), (u64)num_frames);

  if (s_state.muted || s_state.adpcm_muted || g_settings.cdrom_mute_cd_audio)
    return;

  if (xa_codinginfo_is_half_rate(cinfo))
    cdrom_resample_xa_adpcm_18900(sample_buffer, num_frames, xa_codinginfo_is_stereo(cinfo));
  else
    cdrom_resample_xa_adpcm(sample_buffer, num_frames, xa_codinginfo_is_stereo(cinfo));
}

static s16 cdrom_get_peak_volume(const u8* raw_sector, u8 channel)
{
  /* Scan whole 2352-byte sector as s16 samples, return peak of selected
   * channel (0=left, 1=right). */
  const u32 num_samples = CD_IMAGE_RAW_SECTOR_SIZE / sizeof(s16);
  s16 peak = 0;
  for (u32 i = (channel ? 1u : 0u); i < num_samples; i += 2u) {
    s16 v;
    memcpy(&v, raw_sector + i * sizeof(s16), sizeof(v));
    if (v > peak) peak = v;
  }
  return peak;
}

static void cdrom_process_cdda_sector(const u8* raw_sector, const cd_image_subq_t* subq, bool subq_valid)
{
  /* CDDA reporting (only when not in raw read mode): emit an INT1 every
   * time the absolute frame nibble changes. */
  if (s_state.drive_state == CDROM_DRV_PLAYING && s_state.mode.bits.report_audio && subq_valid) {
    if (s_state.cdda_report_start_delay == 0) {
      const u8 frame_nibble = (u8)(subq->absolute_frame_bcd >> 4);
      if (s_state.last_cdda_report_frame_nibble != frame_nibble) {
        s_state.last_cdda_report_frame_nibble = frame_nibble;
        cdrom_clear_async_interrupt();
        byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
        byte_fifo_push(&s_state.async_response_fifo, subq->track_number_bcd);
        byte_fifo_push(&s_state.async_response_fifo, subq->index_number_bcd);
        if (subq->absolute_frame_bcd & 0x10u) {
          byte_fifo_push(&s_state.async_response_fifo, subq->relative_minute_bcd);
          byte_fifo_push(&s_state.async_response_fifo, (u8)(0x80u | subq->relative_second_bcd));
          byte_fifo_push(&s_state.async_response_fifo, subq->relative_frame_bcd);
        } else {
          byte_fifo_push(&s_state.async_response_fifo, subq->absolute_minute_bcd);
          byte_fifo_push(&s_state.async_response_fifo, subq->absolute_second_bcd);
          byte_fifo_push(&s_state.async_response_fifo, subq->absolute_frame_bcd);
        }
        const u8 channel = (u8)(subq->absolute_second_bcd & 1u);
        const s16 peak_volume = cdrom_get_peak_volume(raw_sector, channel);
        const u16 peak_value = (u16)(((u16)channel << 15) | (u16)peak_volume);
        byte_fifo_push(&s_state.async_response_fifo, (u8)(peak_value & 0xFFu));
        byte_fifo_push(&s_state.async_response_fifo, (u8)((peak_value >> 8) & 0xFFu));
        cdrom_set_async_interrupt(CDROM_INT_DATA_READY);
      }
    } else {
      s_state.cdda_report_start_delay--;
    }
  }

  if (s_state.muted || s_state.cdda_auto_pause_pending || g_settings.cdrom_mute_cd_audio)
    return;

  spu_generate_pending_samples();

  /* In 2X mode only half the samples are processed (360 Three Sixty quirk). */
  const u32 num_samples = (CD_IMAGE_RAW_SECTOR_SIZE / sizeof(s16)) / (s_state.mode.bits.double_speed ? 4u : 2u);
  const u32 remaining_space = audio_fifo_space(&s_state.audio_fifo);
  if (remaining_space < num_samples)
    audio_fifo_remove(&s_state.audio_fifo, num_samples - remaining_space);

  const u8* sector_ptr = raw_sector;
  const size_t step = s_state.mode.bits.double_speed ? (sizeof(s16) * 4) : (sizeof(s16) * 2);
  for (u32 i = 0; i < num_samples; i++) {
    s16 samp_left, samp_right;
    memcpy(&samp_left,  sector_ptr,                 sizeof(samp_left));
    memcpy(&samp_right, sector_ptr + sizeof(s16),   sizeof(samp_right));
    sector_ptr += step;
    cdrom_add_audio_frame(samp_left, samp_right);
  }
}

static void cdrom_clear_sector_buffers(void)
{
  s_state.current_read_sector_buffer = 0;
  s_state.current_write_sector_buffer = 0;
  for (u32 i = 0; i < CDROM_NUM_SECTOR_BUFFERS; i++) {
    s_state.sector_buffers[i].position = 0;
    s_state.sector_buffers[i].size = 0;
  }
  s_state.request_register.bits.BFRD = 0;
  s_state.status.bits.DRQSTS = 0;
}

static void cdrom_check_for_sector_buffer_read_complete(void)
{
  cdrom_sector_buffer_t* sb = &s_state.sector_buffers[s_state.current_read_sector_buffer];

  /* BFRD clears when DMA empties the buffer. */
  s_state.request_register.bits.BFRD =
    (s_state.request_register.bits.BFRD && sb->position < sb->size) ? 1u : 0u;
  s_state.status.bits.DRQSTS = s_state.request_register.bits.BFRD;

  /* Maximum/immediate read speedup; wait for data portion read. */
  const u32 data_threshold = s_state.mode.bits.read_raw_sector
                               ? (CDROM_MODE2_HEADER_SIZE + CDROM_DATA_SECTOR_OUTPUT_SIZE)
                               : CDROM_DATA_SECTOR_OUTPUT_SIZE;
  if (s_state.drive_state == CDROM_DRV_READING && sb->position >= data_threshold &&
      cdrom_can_use_read_speedup() && g_settings.cdrom_read_speedup == 0) {
    const tick_count_t remaining = timing_event_get_ticks_until_next_execution(&s_state.drive_event);
    const tick_count_t instant   =
      cdrom_scale_ticks_to_overclock((tick_count_t)g_settings.cdrom_max_read_speedup_cycles);
    if (remaining > instant)
      timing_event_schedule(&s_state.drive_event, instant);
  }

  if (sb->position >= sb->size) {
    sb->position = 0;
    sb->size = 0;
  }

  /* Redeliver missed sector after DMA completion; some games read header
   * then data as separate transfers and expect another INT1 in between. */
  cdrom_sector_buffer_t* next_sb = &s_state.sector_buffers[s_state.current_write_sector_buffer];
  if (next_sb->position == 0 && next_sb->size > 0 && !cdrom_has_pending_async_interrupt() && cdrom_is_reading()) {
    if (cdrom_trace_enabled()) {
      DEV_LOG("[trace] redeliver missed INT1 wr_buf=%u rd_buf=%u",
              s_state.current_write_sector_buffer, s_state.current_read_sector_buffer);
    }
    byte_fifo_push(&s_state.async_response_fifo, s_state.secondary_status.raw);
    s_state.pending_async_interrupt = (u8)CDROM_INT_DATA_READY;
    tick_count_t until = timing_event_get_ticks_until_next_execution(&s_state.drive_event);
    if (until > CDROM_MISSED_INT1_DELAY_CYCLES) until = CDROM_MISSED_INT1_DELAY_CYCLES;
    timing_event_schedule(&s_state.async_interrupt_event, until);
  }
}
