/*
 * See spu.h for the public API.
 *
 * spu_execute(cpu_ticks) is the drive-from-outside entry point: the system
 * tick loop calls it once per CPU slice and the SPU converts those ticks
 * into 44.1 kHz frames.  The transfer FIFO is drained inline.
 *
 * The internal SPSC ring buffer (`s_output_ring`) is the only state shared
 * with the audio thread.  Producer (spu_execute) only touches `head`;
 * consumer (spu_audio_read_frames) only touches `tail`; both load the
 * other end with C11 atomic acquire-release so the audio backend's
 * callback thread never tears.  Capacity is one power-of-two slot of
 * stereo s16 frames.
 */

#include "spu.h"

#include "common/assert.h"
#include "common/bitutils.h"
#include "common/log.h"
#include "common/types.h"

#include "core/settings.h"
#include "core/system.h"
#include "core/timing_event.h"

#include "util/audio_stream.h"
#include "util/core_audio_stream.h"
#include "util/state_wrapper.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(SPU);

/* CDROM mixed-down stereo audio at 44.1kHz; called once per SPU frame.
 * Returns one stereo sample.  cdrom.c provides the real implementation;
 * a weak fallback in this file returns silence so SPU links standalone. */
extern void cdrom_get_audio_frame(s16* left, s16* right);

/* Edge-set the SPU IRQ line on the PS1 interrupt controller. */
extern void interrupt_controller_set_line_state(int irq, bool state);

/* Tell the DMA controller whether the SPU channel wants service. */
extern void dma_set_request(int channel, bool request);

/* Channel index for the SPU on the PS1 DMA; we forward as-is. */
#ifndef SPU_DMA_CHANNEL_INDEX
#  define SPU_DMA_CHANNEL_INDEX 4
#endif
/* IRQ index (matches interrupt_controller_irq_t::SPU = 9). */
#ifndef SPU_IRQ_INDEX
#  define SPU_IRQ_INDEX 9
#endif

/* Weak fallbacks so the SPU object links cleanly while sibling modules
 * are still being ported.  Real implementations override these. */
__attribute__((weak)) void cdrom_get_audio_frame(s16* left, s16* right)
{
  *left = 0;
  *right = 0;
}
__attribute__((weak)) void interrupt_controller_set_line_state(int irq, bool state)
{
  (void)irq;
  (void)state;
}
__attribute__((weak)) void dma_set_request(int channel, bool request)
{
  (void)channel;
  (void)request;
}

enum {
  SPU_BASE                          = 0x1F801C00u,
  NUM_VOICES                        = 24,
  NUM_VOICE_REGISTERS               = 8,
  VOICE_ADDRESS_SHIFT               = 3,
  NUM_SAMPLES_PER_ADPCM_BLOCK       = 28,
  NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK = 3,
  /* MASTER_CLOCK = 33868800; SAMPLE_RATE = 44100; quotient = 0x300. */
  SYSCLK_TICKS_PER_SPU_TICK         = 0x300,
  CAPTURE_BUFFER_SIZE_PER_CHANNEL   = 0x400,
  MINIMUM_TICKS_BETWEEN_KEY_ON_OFF  = 2,
  NUM_REVERB_REGS                   = 32,
  FIFO_SIZE_IN_HALFWORDS            = 32,
  TRANSFER_TICKS_PER_HALFWORD       = 16,
};

typedef enum {
  RAM_TRANSFER_STOPPED      = 0,
  RAM_TRANSFER_MANUAL_WRITE = 1,
  RAM_TRANSFER_DMA_WRITE    = 2,
  RAM_TRANSFER_DMA_READ     = 3,
} ram_transfer_mode_t;

typedef enum {
  ADSR_PHASE_OFF     = 0,
  ADSR_PHASE_ATTACK  = 1,
  ADSR_PHASE_DECAY   = 2,
  ADSR_PHASE_SUSTAIN = 3,
  ADSR_PHASE_RELEASE = 4,
} adsr_phase_t;

typedef union {
  u16 bits;
  struct {
    u16 cd_audio_enable        : 1; /* 0 */
    u16 external_audio_enable  : 1; /* 1 */
    u16 cd_audio_reverb        : 1; /* 2 */
    u16 external_audio_reverb  : 1; /* 3 */
    u16 ram_transfer_mode      : 2; /* 4..5 */
    u16 irq9_enable            : 1; /* 6 */
    u16 reverb_master_enable   : 1; /* 7 */
    u16 noise_clock            : 6; /* 8..13 */
    u16 mute_n                 : 1; /* 14 */
    u16 enable                 : 1; /* 15 */
  } b;
} spucnt_register_t;

typedef union {
  u16 bits;
  struct {
    u16 mode                       : 6; /* 0..5 */
    u16 irq9_flag                  : 1; /* 6 */
    u16 dma_request                : 1; /* 7 */
    u16 dma_read_request           : 1; /* 8 */
    u16 dma_write_request          : 1; /* 9 */
    u16 transfer_busy              : 1; /* 10 */
    u16 second_half_capture_buffer : 1; /* 11 */
    u16 _pad                       : 4; /* 12..15 */
  } b;
} spustat_register_t;

typedef union {
  u16 bits;
} transfer_control_t;

/* ADSR register shape from psx-spx, packed into a u32. */
typedef union {
  u32 bits;
  struct {
    u16 bits_low;
    u16 bits_high;
  };
  struct {
    u32 sustain_level             : 4;  /* 0..3 */
    u32 decay_rate_shr2           : 4;  /* 4..7 */
    u32 attack_rate               : 7;  /* 8..14 */
    u32 attack_exponential        : 1;  /* 15 */
    u32 release_rate_shr2         : 5;  /* 16..20 */
    u32 release_exponential       : 1;  /* 21 */
    u32 sustain_rate              : 7;  /* 22..28 */
    u32 _pad                      : 1;  /* 29 */
    u32 sustain_direction_decrease: 1;  /* 30 */
    u32 sustain_exponential       : 1;  /* 31 */
  } b;
} adsr_register_t;

 /* Per-voice 16-bit volume register.  Two interpretations: fixed-volume
 * (sweep_mode=0) or sweep envelope (sweep_mode=1). */
typedef union {
  u16 bits;
  struct {
    u16 raw_low : 15;
    u16 sweep_mode : 1;
  } common;
  /* sweep_mode == 0 */
  struct {
    s16 fixed_volume_shr1 : 15;
    s16 _sweep_mode       : 1;
  } fixed;
  /* sweep_mode == 1 */
  struct {
    u16 sweep_rate              : 7;  /* 0..6 */
    u16 _unused                 : 5;  /* 7..11 */
    u16 sweep_phase_negative    : 1;  /* 12 */
    u16 sweep_direction_decrease: 1;  /* 13 */
    u16 sweep_exponential       : 1;  /* 14 */
    u16 _sweep_mode             : 1;  /* 15 */
  } sweep;
} volume_register_t;

/* Per-voice register file laid out as both an indexable u16 array and a */
typedef union {
  u16 index[NUM_VOICE_REGISTERS];
  struct {
    volume_register_t volume_left;
    volume_register_t volume_right;
    u16 adpcm_sample_rate;     /* VxPitch */
    u16 adpcm_start_address;   /* multiply by 8 */
    adsr_register_t adsr;
    s16 adsr_volume;
    u16 adpcm_repeat_address;  /* multiply by 8 */
  };
} voice_registers_t;

/* Per-voice play counter; promoted to u32 so it carries across blocks. */
typedef union {
  u32 bits;
  struct {
    u32 _pad0              : 4;  /* 0..3 (sub-interp) */
    u32 interpolation_index: 8;  /* 4..11 */
    u32 sample_index       : 5;  /* 12..16 */
    u32 _pad1              : 15; /* 17..31 */
  } b;
} voice_counter_t;

typedef union {
  u8 bits;
  struct {
    u8 loop_end    : 1;
    u8 loop_repeat : 1;
    u8 loop_start  : 1;
    u8 _pad        : 5;
  } b;
} adpcm_flags_t;

typedef struct {
  union {
    u8 bits;
    struct {
      u8 shift  : 4;
      u8 filter : 4;
    } b;
  } shift_filter;
  adpcm_flags_t flags;
  u8 data[NUM_SAMPLES_PER_ADPCM_BLOCK / 2];
} adpcm_block_t;

/* ADPCM filter shifts above 12 are reserved; PS1 hardware treats them as 9. */
ALWAYS_INLINE static u8 adpcm_block_get_shift(const adpcm_block_t* b)
{
  const u8 shift = b->shift_filter.b.shift;
  return (shift > 12) ? 9 : shift;
}
ALWAYS_INLINE static u8 adpcm_block_get_filter(const adpcm_block_t* b)
{
  return b->shift_filter.b.filter;
}
ALWAYS_INLINE static u8 adpcm_block_get_nibble(const adpcm_block_t* b, u32 index)
{
  return (b->data[index / 2] >> ((index % 2) * 4)) & 0x0F;
}

typedef struct {
  u32  counter;
  u16  counter_increment;
  s16  step;
  u8   rate;
  bool decreasing;
  bool exponential;
  bool phase_invert;
} volume_envelope_t;

enum {
  VOLUME_ENVELOPE_MIN_VOLUME = -32768,
  VOLUME_ENVELOPE_MAX_VOLUME =  32767,
};

typedef struct {
  volume_envelope_t envelope;
  s16  current_level;
  bool envelope_active;
} volume_sweep_t;

typedef struct {
  u16 current_address;
  voice_registers_t regs;
  voice_counter_t counter;
  adpcm_flags_t current_block_flags;
  bool is_first_block;
  /* current_block_samples[0..2] are the previous-block tail kept for
   * 4-tap Gaussian interpolation; [3..30] are the freshly-decoded 28
   * samples of the current ADPCM block. */
  s16 current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK];
  s16 adpcm_last_samples[2];
  s32 last_volume;

  volume_sweep_t left_volume;
  volume_sweep_t right_volume;

  volume_envelope_t adsr_envelope;
  adsr_phase_t adsr_phase;
  s16  adsr_target;
  bool has_samples;
  bool ignore_loop_address;
} spu_voice_t;

typedef struct {
  s16 vLOUT;
  s16 vROUT;
  u16 mBASE;
  union {
    struct {
      u16 FB_SRC_A;
      u16 FB_SRC_B;
      s16 IIR_ALPHA;
      s16 ACC_COEF_A;
      s16 ACC_COEF_B;
      s16 ACC_COEF_C;
      s16 ACC_COEF_D;
      s16 IIR_COEF;
      s16 FB_ALPHA;
      s16 FB_X;
      u16 IIR_DEST_A[2];
      u16 ACC_SRC_A[2];
      u16 ACC_SRC_B[2];
      u16 IIR_SRC_A[2];
      u16 IIR_DEST_B[2];
      u16 ACC_SRC_C[2];
      u16 ACC_SRC_D[2];
      u16 IIR_SRC_B[2];
      u16 MIX_DEST_A[2];
      u16 MIX_DEST_B[2];
      s16 IN_COEF[2];
    };
    u16 rev[NUM_REVERB_REGS];
  };
} reverb_registers_t;

ALWAYS_INLINE static s32 clamp16(s32 v)
{
  return (v < -0x8000) ? -0x8000 : (v > 0x7FFF) ? 0x7FFF : v;
}
ALWAYS_INLINE static s32 apply_volume(s32 sample, s16 volume)
{
  return (sample * (s32)volume) >> 15;
}
ALWAYS_INLINE static s32 clamp_s32(s32 v, s32 lo, s32 hi)
{
  return (v < lo) ? lo : (v > hi) ? hi : v;
}
ALWAYS_INLINE static s32 min_s32(s32 a, s32 b) { return (a < b) ? a : b; }

static void push_sample_to_stream(core_audio_stream_t* stream, s16 left, s16 right)
{
  if (!stream) return;
  s16* dst = NULL;
  u32  cap = 0;
  core_audio_stream_begin_write(stream, &dst, &cap);
  /* Staging buffer always returns >= 1 frame of space (it accumulates 64
   * frames before flushing), so a single-frame write is always safe. */
  if (cap >= 1u && dst) {
    dst[0] = left;
    dst[1] = right;
    core_audio_stream_end_write(stream, 1u);
  }
}

typedef struct {
  u16 data[FIFO_SIZE_IN_HALFWORDS];
  u32 head;
  u32 tail;
  u32 size;
} spu_fifo_t;

static void   fifo_clear (spu_fifo_t* f) { f->head = f->tail = f->size = 0; }
static bool   fifo_empty (const spu_fifo_t* f) { return f->size == 0; }
static bool   fifo_full  (const spu_fifo_t* f) { return f->size == FIFO_SIZE_IN_HALFWORDS; }
static u32    fifo_size  (const spu_fifo_t* f) { return f->size; }
static u32    fifo_space (const spu_fifo_t* f) { return FIFO_SIZE_IN_HALFWORDS - f->size; }
static void   fifo_push  (spu_fifo_t* f, u16 v)
{
  f->data[f->tail] = v;
  f->tail = (f->tail + 1u) % FIFO_SIZE_IN_HALFWORDS;
  f->size++;
}
static u16    fifo_pop   (spu_fifo_t* f)
{
  const u16 v = f->data[f->head];
  f->head = (f->head + 1u) % FIFO_SIZE_IN_HALFWORDS;
  f->size--;
  return v;
}
static void   fifo_push_range(spu_fifo_t* f, const u16* src, u32 count)
{
  for (u32 i = 0; i < count; i++)
    fifo_push(f, src[i]);
}
static void   fifo_pop_range (spu_fifo_t* f, u16* dst, u32 count)
{
  for (u32 i = 0; i < count; i++)
    dst[i] = fifo_pop(f);
}

typedef struct {
  /* Timing state.  Driven by `tick_event`, a periodic timing_event registered
   * with the global scheduler. Per-halfword transfer is still advanced inline
   * inside spu_execute() via transfer_ticks_carry. */
  tick_count_t ticks_carry;
  tick_count_t cpu_ticks_per_spu_tick;
  tick_count_t cpu_tick_divider;
  tick_count_t transfer_ticks_carry;
  bool         transfer_event_active;
  timing_event_t tick_event;

  spucnt_register_t  SPUCNT;
  spustat_register_t SPUSTAT;

  transfer_control_t transfer_control;
  u16 transfer_address_reg;
  u32 transfer_address;

  u16 irq_address;
  u16 capture_buffer_position;

  volume_register_t main_volume_left_reg;
  volume_register_t main_volume_right_reg;
  volume_sweep_t    main_volume_left;
  volume_sweep_t    main_volume_right;

  s16 cd_audio_volume_left;
  s16 cd_audio_volume_right;

  s16 external_volume_left;
  s16 external_volume_right;

  u32 key_on_register;
  u32 key_off_register;
  u32 endx_register;
  u32 pitch_modulation_enable_register;

  u32 noise_mode_register;
  u32 noise_count;
  u32 noise_level;

  u32 reverb_on_register;
  u32 reverb_base_address;
  u32 reverb_current_address;
  reverb_registers_t reverb_registers;
  /* Resampling buffers are duplicated to dodge the wrap-around in the */
  s16 reverb_downsample_buffer[2][128];
  s16 reverb_upsample_buffer[2][64];
  s32 reverb_resample_buffer_position;

  s16 last_reverb_input[2];
  s32 last_reverb_output[2];
  bool audio_output_muted;

  spu_voice_t voices[NUM_VOICES];

  spu_fifo_t transfer_fifo;

  core_audio_stream_t* audio_stream;
} spu_state_t;

static ALIGN_TO_CACHE_LINE spu_state_t s_state;
static ALIGN_TO_CACHE_LINE u8          s_ram[SPU_RAM_SIZE];

typedef struct {
  u64 reg_writes_kon_lo;       /* 0x1F801D88 */
  u64 reg_writes_kon_hi;       /* 0x1F801D8A */
  u64 reg_writes_koff_lo;      /* 0x1F801D8C */
  u64 reg_writes_koff_hi;      /* 0x1F801D8E */
  u64 reg_writes_spucnt;       /* 0x1F801DAA */
  u64 reg_writes_xfer_addr;    /* 0x1F801DA6 */
  u64 reg_writes_xfer_data;    /* 0x1F801DA8 (manual) */
  u64 voice_key_on_calls;
  u64 voice_key_off_calls;
  u64 manual_xfer_writes;
  u64 dma_write_blocks;
  u64 dma_write_halfwords;
  u64 dma_read_blocks;
  u64 dma_read_halfwords;
  u64 spu_execute_calls;
  u64 frames_generated;
  u64 ring_push_frames;
  u64 ring_push_nonzero_frames;
  u32 last_spucnt_seen;
  u32 max_voices_on_seen;
} spu_dbg_t;

static spu_dbg_t s_spu_dbg;

void spu_dbg_dump_counters(FILE* fp)
{
  fprintf(fp,
    "----- SPU debug counters -----\n"
    "  reg writes:        KONlo=%llu KONhi=%llu KOFFlo=%llu KOFFhi=%llu\n"
    "                     SPUCNT=%llu XFERaddr=%llu XFERdata=%llu\n"
    "  voice keying:      key_on=%llu key_off=%llu  max_voices_on=%u\n"
    "  manual xfer:       writes=%llu\n"
    "  DMA ch4:           write_blocks=%llu write_hw=%llu  read_blocks=%llu read_hw=%llu\n"
    "  spu_execute:       calls=%llu  frames=%llu\n"
    "  ring output:       pushed=%llu  nonzero=%llu\n"
    "  last SPUCNT seen:  0x%04X (enable=%u mute_n=%u xfer=%u irq9=%u cd=%u)\n",
    (unsigned long long)s_spu_dbg.reg_writes_kon_lo,
    (unsigned long long)s_spu_dbg.reg_writes_kon_hi,
    (unsigned long long)s_spu_dbg.reg_writes_koff_lo,
    (unsigned long long)s_spu_dbg.reg_writes_koff_hi,
    (unsigned long long)s_spu_dbg.reg_writes_spucnt,
    (unsigned long long)s_spu_dbg.reg_writes_xfer_addr,
    (unsigned long long)s_spu_dbg.reg_writes_xfer_data,
    (unsigned long long)s_spu_dbg.voice_key_on_calls,
    (unsigned long long)s_spu_dbg.voice_key_off_calls,
    s_spu_dbg.max_voices_on_seen,
    (unsigned long long)s_spu_dbg.manual_xfer_writes,
    (unsigned long long)s_spu_dbg.dma_write_blocks,
    (unsigned long long)s_spu_dbg.dma_write_halfwords,
    (unsigned long long)s_spu_dbg.dma_read_blocks,
    (unsigned long long)s_spu_dbg.dma_read_halfwords,
    (unsigned long long)s_spu_dbg.spu_execute_calls,
    (unsigned long long)s_spu_dbg.frames_generated,
    (unsigned long long)s_spu_dbg.ring_push_frames,
    (unsigned long long)s_spu_dbg.ring_push_nonzero_frames,
    (unsigned)(s_spu_dbg.last_spucnt_seen & 0xFFFFu),
    (unsigned)((s_spu_dbg.last_spucnt_seen >> 15) & 1u),
    (unsigned)((s_spu_dbg.last_spucnt_seen >> 14) & 1u),
    (unsigned)((s_spu_dbg.last_spucnt_seen >>  4) & 3u),
    (unsigned)((s_spu_dbg.last_spucnt_seen >>  6) & 1u), 
    (unsigned)((s_spu_dbg.last_spucnt_seen >>  0) & 1u));
  if (s_state.audio_stream) {
    fprintf(fp,
      "  audio output:      core_audio_stream attached, buffered=%u target=%u\n",
      core_audio_stream_get_buffered_frames_relaxed(s_state.audio_stream),
      core_audio_stream_get_target_buffer_size     (s_state.audio_stream));
  } else {
    fprintf(fp, "  audio output:      <no stream>\n");
  }
  fflush(fp);
}

static void update_event_interval(void);
static void update_dma_request(void);
static void update_transfer_event(void);
static void execute_transfer(tick_count_t ticks);
static void manual_transfer_write(u16 value);

static u16  read_voice_register (u32 offset);
static void write_voice_register(u32 offset, u16 value);

static bool is_voice_reverb_enabled (u32 i);
static bool is_voice_noise_enabled  (u32 i);
static bool is_pitch_modulation_enabled(u32 i);
static s16  get_voice_noise_level   (void);

static bool is_ram_irq_triggerable(void);
static bool check_ram_irq         (u32 address);
static void trigger_ram_irq       (void);
static void check_for_late_ram_irqs(void);

static void write_to_capture_buffer        (u32 index, s16 value);
static void increment_capture_buffer_position(void);

static void read_adpcm_block (u16 address, adpcm_block_t* block);
static void sample_voice     (u32 voice_index, s32* out_left, s32* out_right);

static void update_noise(void);

static u32  reverb_memory_address(u32 address);
static s16  reverb_read   (u32 address, s32 offset);
static void reverb_write  (u32 address, s16 data);
static void process_reverb(s32 left_in, s32 right_in, s32* left_out, s32* right_out);

static void internal_generate_pending_samples(void);

static void voice_key_on (spu_voice_t* v);
static void voice_key_off(spu_voice_t* v);
static void voice_force_off(spu_voice_t* v);
static void voice_decode_block(spu_voice_t* v, const adpcm_block_t* block);
static s32  voice_interpolate(const spu_voice_t* v);
static void voice_update_adsr_envelope(spu_voice_t* v);
static void voice_tick_adsr (spu_voice_t* v);

static void volume_envelope_reset(volume_envelope_t* e, u8 rate, u8 rate_mask,
                                  bool decreasing, bool exponential, bool phase_invert);
static bool volume_envelope_tick(volume_envelope_t* e, s16* current_level);
static void volume_sweep_reset(volume_sweep_t* s, volume_register_t reg);
static void volume_sweep_tick (volume_sweep_t* s);

ALWAYS_INLINE static bool voice_is_on(const spu_voice_t* v) { return v->adsr_phase != ADSR_PHASE_OFF; }

static void spu_tick_trampoline(void* user, tick_count_t ticks, tick_count_t ticks_late)
{
  (void)user; (void)ticks_late;
  spu_execute(ticks);
}

void spu_initialize(void)
{

  s_state.cpu_ticks_per_spu_tick = system_scale_ticks_to_overclock(SYSCLK_TICKS_PER_SPU_TICK);
  s_state.cpu_tick_divider       = (tick_count_t)(g_settings.cpu_overclock_numerator
                                                  * (u32)SYSCLK_TICKS_PER_SPU_TICK);

  /* Note: do NOT clear s_state.audio_stream here.  The frontend binds the
   * core_audio_stream via spu_set_output_stream() before any boot, and
   * spu_initialize() runs as part of system_initialize() which happens
   * AFTER audio_setup(). */

  /* Drive spu_execute() from the global timing-event scheduler. Period is one
   * audio batch (256 samples ~= 5.8 ms); coarse enough that the event-rate
   * stays low, fine enough for IRQ + SPU envelope cadence to land correctly.
   * Period uses scaled cpu_ticks_per_spu_tick so the event fires at correct
   * wall-clock cadence under overclock. */
  static const char k_name[] = "SPU Sample";
  const tick_count_t period = s_state.cpu_ticks_per_spu_tick * 256;
  timing_event_init(&s_state.tick_event, k_name, sizeof(k_name) - 1,
                    period, period, spu_tick_trampoline, NULL);
  timing_event_activate(&s_state.tick_event);

  spu_reset();
}

void spu_cpu_clock_changed(void)
{
  s_state.cpu_ticks_per_spu_tick = system_scale_ticks_to_overclock(SYSCLK_TICKS_PER_SPU_TICK);
  s_state.cpu_tick_divider       = (tick_count_t)(g_settings.cpu_overclock_numerator
                                                  * (u32)SYSCLK_TICKS_PER_SPU_TICK);

  s_state.ticks_carry            = 0;
  update_event_interval();
}

void spu_shutdown(void)
{
  if (timing_event_is_active(&s_state.tick_event))
    timing_event_deactivate(&s_state.tick_event);
  timing_event_destroy(&s_state.tick_event);
  s_state.audio_stream = NULL;
}

void spu_reset(void)
{
  s_state.ticks_carry = 0;
  s_state.transfer_ticks_carry = 0;
  s_state.transfer_event_active = false;

  s_state.SPUCNT.bits  = 0;
  s_state.SPUSTAT.bits = 0;
  s_state.transfer_address     = 0;
  s_state.transfer_address_reg = 0;
  s_state.irq_address          = 0;
  s_state.capture_buffer_position = 0;
  s_state.main_volume_left_reg.bits  = 0;
  s_state.main_volume_right_reg.bits = 0;
  memset(&s_state.main_volume_left,  0, sizeof(s_state.main_volume_left));
  memset(&s_state.main_volume_right, 0, sizeof(s_state.main_volume_right));
  s_state.cd_audio_volume_left = 0;
  s_state.cd_audio_volume_right = 0;
  s_state.external_volume_left = 0;
  s_state.external_volume_right = 0;
  s_state.key_on_register  = 0;
  s_state.key_off_register = 0;
  s_state.endx_register    = 0;
  s_state.pitch_modulation_enable_register = 0;

  s_state.noise_mode_register = 0;
  s_state.noise_count = 0;
  s_state.noise_level = 1;

  s_state.reverb_on_register = 0;
  memset(&s_state.reverb_registers, 0, sizeof(s_state.reverb_registers));
  s_state.reverb_registers.mBASE = 0;
  s_state.reverb_base_address    = ((u32)s_state.reverb_registers.mBASE) << 2;
  s_state.reverb_current_address = s_state.reverb_base_address;
  memset(s_state.reverb_downsample_buffer, 0, sizeof(s_state.reverb_downsample_buffer));
  memset(s_state.reverb_upsample_buffer,   0, sizeof(s_state.reverb_upsample_buffer));
  s_state.reverb_resample_buffer_position = 0;

  for (u32 i = 0; i < NUM_VOICES; i++) {
    spu_voice_t* v = &s_state.voices[i];
    v->current_address = 0;
    memset(v->regs.index, 0, sizeof(v->regs.index));
    v->counter.bits = 0;
    v->current_block_flags.bits = 0;
    v->is_first_block = false;
    memset(v->current_block_samples, 0, sizeof(v->current_block_samples));
    memset(v->adpcm_last_samples,    0, sizeof(v->adpcm_last_samples));
    v->last_volume = 0;
    memset(&v->left_volume,  0, sizeof(v->left_volume));
    memset(&v->right_volume, 0, sizeof(v->right_volume));
    volume_envelope_reset(&v->adsr_envelope, 0, 0, false, false, false);
    v->adsr_phase = ADSR_PHASE_OFF;
    v->adsr_target = 0;
    v->has_samples = false;
    v->ignore_loop_address = false;
  }

  fifo_clear(&s_state.transfer_fifo);
  memset(s_ram, 0, SPU_RAM_SIZE);

  /* Deactivate before rescheduling so timing_event_schedule takes the
   * inactive->active path and resets last_run_time.  Without this, a warm
   * reset preserves the pre-reset last_run_time; after timing_events_reset
   * zeros global_tick_counter the first post-reset SPU fire computes
   * ticks_to_execute = small_global - huge_pre_reset_last, underflows to a
   * huge negative s32, which spu_execute interprets as ~4 billion frames to
   * generate. CPU starves, CRTC tick never fires, frame_done stops, hang. */
  timing_event_deactivate(&s_state.tick_event);
  update_event_interval();
}

bool spu_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_s32(sw, &s_state.ticks_carry);
  state_wrapper_do_u16(sw, &s_state.SPUCNT.bits);
  state_wrapper_do_u16(sw, &s_state.SPUSTAT.bits);
  state_wrapper_do_u16(sw, &s_state.transfer_control.bits);
  state_wrapper_do_u32(sw, &s_state.transfer_address);
  state_wrapper_do_u16(sw, &s_state.transfer_address_reg);
  state_wrapper_do_u16(sw, &s_state.irq_address);
  state_wrapper_do_u16(sw, &s_state.capture_buffer_position);
  state_wrapper_do_u16(sw, &s_state.main_volume_left_reg.bits);
  state_wrapper_do_u16(sw, &s_state.main_volume_right_reg.bits);
  state_wrapper_do_array(sw, &s_state.main_volume_left,  sizeof(s_state.main_volume_left),  1);
  state_wrapper_do_array(sw, &s_state.main_volume_right, sizeof(s_state.main_volume_right), 1);
  state_wrapper_do_s16(sw, &s_state.cd_audio_volume_left);
  state_wrapper_do_s16(sw, &s_state.cd_audio_volume_right);
  state_wrapper_do_s16(sw, &s_state.external_volume_left);
  state_wrapper_do_s16(sw, &s_state.external_volume_right);
  state_wrapper_do_u32(sw, &s_state.key_on_register);
  state_wrapper_do_u32(sw, &s_state.key_off_register);
  state_wrapper_do_u32(sw, &s_state.endx_register);
  state_wrapper_do_u32(sw, &s_state.pitch_modulation_enable_register);
  state_wrapper_do_u32(sw, &s_state.noise_mode_register);
  state_wrapper_do_u32(sw, &s_state.noise_count);
  state_wrapper_do_u32(sw, &s_state.noise_level);
  state_wrapper_do_u32(sw, &s_state.reverb_on_register);
  state_wrapper_do_u32(sw, &s_state.reverb_base_address);
  state_wrapper_do_u32(sw, &s_state.reverb_current_address);
  state_wrapper_do_s16(sw, &s_state.reverb_registers.vLOUT);
  state_wrapper_do_s16(sw, &s_state.reverb_registers.vROUT);
  state_wrapper_do_u16(sw, &s_state.reverb_registers.mBASE);
  state_wrapper_do_array(sw, s_state.reverb_registers.rev, sizeof(u16), NUM_REVERB_REGS);
  state_wrapper_do_array(sw, s_state.reverb_downsample_buffer,
                         sizeof(s_state.reverb_downsample_buffer), 1);
  state_wrapper_do_array(sw, s_state.reverb_upsample_buffer,
                         sizeof(s_state.reverb_upsample_buffer), 1);
  state_wrapper_do_s32(sw, &s_state.reverb_resample_buffer_position);

  for (u32 i = 0; i < NUM_VOICES; i++) {
    spu_voice_t* v = &s_state.voices[i];
    state_wrapper_do_u16(sw, &v->current_address);
    state_wrapper_do_array(sw, v->regs.index, sizeof(u16), NUM_VOICE_REGISTERS);
    state_wrapper_do_u32(sw, &v->counter.bits);
    state_wrapper_do_u8 (sw, &v->current_block_flags.bits);
    state_wrapper_do_bool(sw, &v->is_first_block);
    state_wrapper_do_array(sw, &v->current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK],
                           sizeof(s16), NUM_SAMPLES_PER_ADPCM_BLOCK);
    state_wrapper_do_array(sw, &v->current_block_samples[0],
                           sizeof(s16), NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK);
    state_wrapper_do_array(sw, v->adpcm_last_samples, sizeof(s16), 2);
    state_wrapper_do_s32(sw, &v->last_volume);
    state_wrapper_do_array(sw, &v->left_volume,  sizeof(v->left_volume),  1);
    state_wrapper_do_array(sw, &v->right_volume, sizeof(v->right_volume), 1);
    state_wrapper_do_array(sw, &v->adsr_envelope, sizeof(v->adsr_envelope), 1);
    {
      u8 phase = (u8)v->adsr_phase;
      state_wrapper_do_u8(sw, &phase);
      v->adsr_phase = (adsr_phase_t)phase;
    }
    state_wrapper_do_s16(sw, &v->adsr_target);
    state_wrapper_do_bool(sw, &v->has_samples);
    state_wrapper_do_bool(sw, &v->ignore_loop_address);
  }

  /* FIFO: serialize the contents and head/tail/size. */
  state_wrapper_do_array(sw, s_state.transfer_fifo.data, sizeof(u16), FIFO_SIZE_IN_HALFWORDS);
  state_wrapper_do_u32(sw, &s_state.transfer_fifo.head);
  state_wrapper_do_u32(sw, &s_state.transfer_fifo.tail);
  state_wrapper_do_u32(sw, &s_state.transfer_fifo.size);

  state_wrapper_do_bytes(sw, s_ram, SPU_RAM_SIZE);

  if (state_wrapper_is_reading(sw)) {
    update_event_interval();
    update_transfer_event();
  }

  return !state_wrapper_has_error(sw);
}

u16 spu_read_register(u32 offset)
{
  switch (offset) {
    case 0x1F801D80u - SPU_BASE: return s_state.main_volume_left_reg.bits;
    case 0x1F801D82u - SPU_BASE: return s_state.main_volume_right_reg.bits;
    case 0x1F801D84u - SPU_BASE: return (u16)s_state.reverb_registers.vLOUT;
    case 0x1F801D86u - SPU_BASE: return (u16)s_state.reverb_registers.vROUT;
    case 0x1F801D88u - SPU_BASE: return Truncate16(s_state.key_on_register);
    case 0x1F801D8Au - SPU_BASE: return Truncate16(s_state.key_on_register >> 16);
    case 0x1F801D8Cu - SPU_BASE: return Truncate16(s_state.key_off_register);
    case 0x1F801D8Eu - SPU_BASE: return Truncate16(s_state.key_off_register >> 16);
    case 0x1F801D90u - SPU_BASE: return Truncate16(s_state.pitch_modulation_enable_register);
    case 0x1F801D92u - SPU_BASE: return Truncate16(s_state.pitch_modulation_enable_register >> 16);
    case 0x1F801D94u - SPU_BASE: return Truncate16(s_state.noise_mode_register);
    case 0x1F801D96u - SPU_BASE: return Truncate16(s_state.noise_mode_register >> 16);
    case 0x1F801D98u - SPU_BASE: return Truncate16(s_state.reverb_on_register);
    case 0x1F801D9Au - SPU_BASE: return Truncate16(s_state.reverb_on_register >> 16);
    case 0x1F801D9Cu - SPU_BASE: return Truncate16(s_state.endx_register);
    case 0x1F801D9Eu - SPU_BASE: return Truncate16(s_state.endx_register >> 16);
    case 0x1F801DA2u - SPU_BASE: return s_state.reverb_registers.mBASE;
    case 0x1F801DA4u - SPU_BASE: return s_state.irq_address;
    case 0x1F801DA6u - SPU_BASE: return s_state.transfer_address_reg;
    case 0x1F801DA8u - SPU_BASE: return UINT16_C(0xFFFF);
    case 0x1F801DAAu - SPU_BASE: return s_state.SPUCNT.bits;
    case 0x1F801DACu - SPU_BASE: return s_state.transfer_control.bits;
    case 0x1F801DAEu - SPU_BASE:
      spu_generate_pending_samples();
      return s_state.SPUSTAT.bits;
    case 0x1F801DB0u - SPU_BASE: return (u16)s_state.cd_audio_volume_left;
    case 0x1F801DB2u - SPU_BASE: return (u16)s_state.cd_audio_volume_right;
    case 0x1F801DB4u - SPU_BASE: return (u16)s_state.external_volume_left;
    case 0x1F801DB6u - SPU_BASE: return (u16)s_state.external_volume_right;
    case 0x1F801DB8u - SPU_BASE:
      spu_generate_pending_samples();
      return (u16)s_state.main_volume_left.current_level;
    case 0x1F801DBAu - SPU_BASE:
      spu_generate_pending_samples();
      return (u16)s_state.main_volume_right.current_level;
    default: {
      if (offset < (0x1F801D80u - SPU_BASE))
        return read_voice_register(offset);

      if (offset >= (0x1F801DC0u - SPU_BASE) && offset < (0x1F801E00u - SPU_BASE))
        return s_state.reverb_registers.rev[(offset - (0x1F801DC0u - SPU_BASE)) / 2u];

      if (offset >= (0x1F801E00u - SPU_BASE) && offset < (0x1F801E60u - SPU_BASE)) {
        const u32 voice_index = (offset - (0x1F801E00u - SPU_BASE)) / 4u;
        spu_generate_pending_samples();
        if (offset & 0x02u)
          return (u16)s_state.voices[voice_index].left_volume.current_level;
        return (u16)s_state.voices[voice_index].right_volume.current_level;
      }

      DEV_LOG("Unknown SPU register read: offset 0x%X (address 0x%08X)", offset, offset | SPU_BASE);
      return UINT16_C(0xFFFF);
    }
  }
}

void spu_write_register(u32 offset, u16 value)
{
  switch (offset) {
    case 0x1F801D80u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.main_volume_left_reg.bits = value;
      volume_sweep_reset(&s_state.main_volume_left, s_state.main_volume_left_reg);
      return;

    case 0x1F801D82u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.main_volume_right_reg.bits = value;
      volume_sweep_reset(&s_state.main_volume_right, s_state.main_volume_right_reg);
      return;

    case 0x1F801D84u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.reverb_registers.vLOUT = (s16)value;
      return;

    case 0x1F801D86u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.reverb_registers.vROUT = (s16)value;
      return;

    case 0x1F801D88u - SPU_BASE:
      s_spu_dbg.reg_writes_kon_lo++;
      spu_generate_pending_samples();
      s_state.key_on_register = (s_state.key_on_register & 0xFFFF0000u) | ZeroExtend32(value);
      return;

    case 0x1F801D8Au - SPU_BASE:
      s_spu_dbg.reg_writes_kon_hi++;
      spu_generate_pending_samples();
      s_state.key_on_register = (s_state.key_on_register & 0x0000FFFFu) | (ZeroExtend32(value) << 16);
      return;

    case 0x1F801D8Cu - SPU_BASE:
      s_spu_dbg.reg_writes_koff_lo++;
      spu_generate_pending_samples();
      s_state.key_off_register = (s_state.key_off_register & 0xFFFF0000u) | ZeroExtend32(value);
      return;

    case 0x1F801D8Eu - SPU_BASE:
      s_spu_dbg.reg_writes_koff_hi++;
      spu_generate_pending_samples();
      s_state.key_off_register = (s_state.key_off_register & 0x0000FFFFu) | (ZeroExtend32(value) << 16);
      return;

    case 0x1F801D90u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.pitch_modulation_enable_register =
        (s_state.pitch_modulation_enable_register & 0xFFFF0000u) | ZeroExtend32(value);
      return;

    case 0x1F801D92u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.pitch_modulation_enable_register =
        (s_state.pitch_modulation_enable_register & 0x0000FFFFu) | (ZeroExtend32(value) << 16);
      return;

    case 0x1F801D94u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.noise_mode_register = (s_state.noise_mode_register & 0xFFFF0000u) | ZeroExtend32(value);
      return;

    case 0x1F801D96u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.noise_mode_register = (s_state.noise_mode_register & 0x0000FFFFu) | (ZeroExtend32(value) << 16);
      return;

    case 0x1F801D98u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.reverb_on_register = (s_state.reverb_on_register & 0xFFFF0000u) | ZeroExtend32(value);
      return;

    case 0x1F801D9Au - SPU_BASE:
      spu_generate_pending_samples();
      s_state.reverb_on_register = (s_state.reverb_on_register & 0x0000FFFFu) | (ZeroExtend32(value) << 16);
      return;

    case 0x1F801DA2u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.reverb_registers.mBASE = value;
      s_state.reverb_base_address    = ZeroExtend32(value << 2) & 0x3FFFFu;
      s_state.reverb_current_address = s_state.reverb_base_address;
      return;

    case 0x1F801DA4u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.irq_address = value;
      if (is_ram_irq_triggerable())
        check_for_late_ram_irqs();
      return;

    case 0x1F801DA6u - SPU_BASE:
      s_spu_dbg.reg_writes_xfer_addr++;
      /* Drain pending transfer ticks before resetting address. */
      execute_transfer(INT32_MAX);
      s_state.transfer_address_reg = value;
      s_state.transfer_address     = ZeroExtend32(value) * 8u;
      if (is_ram_irq_triggerable() && check_ram_irq(s_state.transfer_address))
        trigger_ram_irq();
      return;

    case 0x1F801DA8u - SPU_BASE:
      s_spu_dbg.reg_writes_xfer_data++;
      manual_transfer_write(value);
      return;

    case 0x1F801DAAu - SPU_BASE: {
      s_spu_dbg.reg_writes_spucnt++;
      s_spu_dbg.last_spucnt_seen = value;
      spu_generate_pending_samples();
      const spucnt_register_t new_value = { .bits = value };
      if (new_value.b.ram_transfer_mode != s_state.SPUCNT.b.ram_transfer_mode &&
          new_value.b.ram_transfer_mode == RAM_TRANSFER_STOPPED) {
        if (!fifo_empty(&s_state.transfer_fifo)) {
          if (s_state.SPUCNT.b.ram_transfer_mode == RAM_TRANSFER_DMA_WRITE) {
            /* Drain rather than emulate the slow per-halfword writeback. */
            WARNING_LOG("Draining write SPU transfer FIFO with %u halfwords left",
                        fifo_size(&s_state.transfer_fifo));
            execute_transfer(INT32_MAX);
          } else {
            fifo_clear(&s_state.transfer_fifo);
          }
        }
      }

      if (!new_value.b.enable && s_state.SPUCNT.b.enable) {
        for (u32 i = 0; i < NUM_VOICES; i++)
          voice_force_off(&s_state.voices[i]);
      }

      s_state.SPUCNT.bits = new_value.bits;
      /* SPUSTAT mirrors the low 6 bits of SPUCNT (cd/ext audio + transfer mode). */
      s_state.SPUSTAT.b.mode = s_state.SPUCNT.bits & 0x3Fu;

      if (!s_state.SPUCNT.b.irq9_enable) {
        s_state.SPUSTAT.b.irq9_flag = 0;
        interrupt_controller_set_line_state(SPU_IRQ_INDEX, false);
      } else if (is_ram_irq_triggerable()) {
        check_for_late_ram_irqs();
      }

      update_event_interval();
      update_dma_request();
      update_transfer_event();
      return;
    }

    case 0x1F801DACu - SPU_BASE:
      s_state.transfer_control.bits = value;
      return;

    case 0x1F801DB0u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.cd_audio_volume_left = (s16)value;
      return;

    case 0x1F801DB2u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.cd_audio_volume_right = (s16)value;
      return;

    case 0x1F801DB4u - SPU_BASE:
      s_state.external_volume_left = (s16)value;
      return;
    case 0x1F801DB6u - SPU_BASE:
      s_state.external_volume_right = (s16)value;
      return;

    case 0x1F801DB8u - SPU_BASE:
      spu_generate_pending_samples();
      s_state.main_volume_left.current_level = (s16)value;
      return;
    case 0x1F801DBAu - SPU_BASE:
      spu_generate_pending_samples();
      s_state.main_volume_right.current_level = (s16)value;
      return;

    case 0x1F801DAEu - SPU_BASE:
      /* SPUSTAT is read-only. */
      return;

    case 0x1F801D9Cu - SPU_BASE:
    case 0x1F801D9Eu - SPU_BASE:
      /* ENDX (voice end-flag status) is read-only; ignore writes. */
      return;

    default: {
      if (offset < (0x1F801D80u - SPU_BASE)) {
        write_voice_register(offset, value);
        return;
      }
      if (offset >= (0x1F801DC0u - SPU_BASE) && offset < (0x1F801E00u - SPU_BASE)) {
        const u32 reg = (offset - (0x1F801DC0u - SPU_BASE)) / 2u;
        spu_generate_pending_samples();
        s_state.reverb_registers.rev[reg] = value;
        return;
      }
      DEV_LOG("Unknown SPU register write: offset 0x%X value 0x%04X", offset, value);
      return;
    }
  }
}

static u16 read_voice_register(u32 offset)
{
  const u32 reg_index   = (offset % 0x10u) / 2u;
  const u32 voice_index = offset / 0x10u;
  Assert(voice_index < NUM_VOICES);

  const spu_voice_t* voice = &s_state.voices[voice_index];
  if (reg_index >= 6 && (voice_is_on(voice) || (s_state.key_on_register & (1u << voice_index))))
    spu_generate_pending_samples();

  return voice->regs.index[reg_index];
}

static void write_voice_register(u32 offset, u16 value)
{
  const u32 reg_index   = offset % 0x10u;
  const u32 voice_index = offset / 0x10u;
  DebugAssert(voice_index < NUM_VOICES);

  spu_voice_t* voice = &s_state.voices[voice_index];
  if (voice_is_on(voice) || (s_state.key_on_register & (1u << voice_index)))
    spu_generate_pending_samples();

  switch (reg_index) {
    case 0x00:
      voice->regs.volume_left.bits = value;
      volume_sweep_reset(&voice->left_volume, voice->regs.volume_left);
      return;
    case 0x02:
      voice->regs.volume_right.bits = value;
      volume_sweep_reset(&voice->right_volume, voice->regs.volume_right);
      return;
    case 0x04:
      voice->regs.adpcm_sample_rate = value;
      return;
    case 0x06:
      voice->regs.adpcm_start_address = value;
      return;
    case 0x08:
      voice->regs.adsr.bits_low = value;
      if (voice_is_on(voice))
        voice_update_adsr_envelope(voice);
      return;
    case 0x0A:
      voice->regs.adsr.bits_high = value;
      if (voice_is_on(voice))
        voice_update_adsr_envelope(voice);
      return;
    case 0x0C:
      voice->regs.adsr_volume = (s16)value;
      return;
    case 0x0E: {
      /* This register lets games override the loop pointer baked into ADPCM
       * block headers, but games (Misadventures of Tron Bonne, Re-Loaded,
       * Valkyrie Profile) rely on a small window where the override is *not*
       * respected. */
      const bool ignore_loop_address = !voice_is_on(voice) || !voice->is_first_block;
      voice->regs.adpcm_repeat_address = value;
      voice->ignore_loop_address |= ignore_loop_address;
      return;
    }
    default:
      ERROR_LOG("Unknown SPU voice %u register write: reg 0x%X value 0x%04X",
                voice_index, reg_index, value);
      return;
  }
}

static bool is_voice_reverb_enabled (u32 i) { return ((s_state.reverb_on_register             >> i) & 1u) != 0; }
static bool is_voice_noise_enabled  (u32 i) { return ((s_state.noise_mode_register            >> i) & 1u) != 0; }
static bool is_pitch_modulation_enabled(u32 i)
{
  return (i > 0) && (((s_state.pitch_modulation_enable_register >> i) & 1u) != 0);
}
static s16  get_voice_noise_level   (void)  { return (s16)(u16)s_state.noise_level; }

static bool is_ram_irq_triggerable(void)
{
  return s_state.SPUCNT.b.irq9_enable && !s_state.SPUSTAT.b.irq9_flag;
}
static bool check_ram_irq(u32 address)
{
  return (ZeroExtend32(s_state.irq_address) * 8u) == address;
}
static void trigger_ram_irq(void)
{
  s_state.SPUSTAT.b.irq9_flag = 1;
  interrupt_controller_set_line_state(SPU_IRQ_INDEX, true);
}
static void check_for_late_ram_irqs(void)
{
  if (check_ram_irq(s_state.transfer_address)) {
    trigger_ram_irq();
    return;
  }
  for (u32 i = 0; i < NUM_VOICES; i++) {
    const spu_voice_t* v = &s_state.voices[i];
    if (!v->has_samples)
      continue;
    const u32 address = (u32)v->current_address * 8u;
    if (check_ram_irq(address) || check_ram_irq((address + 8u) & SPU_RAM_MASK)) {
      trigger_ram_irq();
      return;
    }
  }
}

static void write_to_capture_buffer(u32 index, s16 value)
{
  const u32 ram_address =
    (index * (u32)CAPTURE_BUFFER_SIZE_PER_CHANNEL) | ZeroExtend32_u16((u16)s_state.capture_buffer_position);
  memcpy(&s_ram[ram_address], &value, sizeof(value));
  if (is_ram_irq_triggerable() && check_ram_irq(ram_address))
    trigger_ram_irq();
}
static void increment_capture_buffer_position(void)
{
  s_state.capture_buffer_position += sizeof(s16);
  s_state.capture_buffer_position %= CAPTURE_BUFFER_SIZE_PER_CHANNEL;
  s_state.SPUSTAT.b.second_half_capture_buffer =
    (s_state.capture_buffer_position >= (CAPTURE_BUFFER_SIZE_PER_CHANNEL / 2)) ? 1 : 0;
}

static void execute_fifo_read_from_ram(tick_count_t* ticks)
{
  while (*ticks > 0 && !fifo_full(&s_state.transfer_fifo)) {
    u16 value;
    memcpy(&value, &s_ram[s_state.transfer_address], sizeof(u16));
    s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & SPU_RAM_MASK;
    fifo_push(&s_state.transfer_fifo, value);
    *ticks -= TRANSFER_TICKS_PER_HALFWORD;
    if (is_ram_irq_triggerable() && check_ram_irq(s_state.transfer_address))
      trigger_ram_irq();
  }
}

static void execute_fifo_write_to_ram(tick_count_t* ticks)
{
  while (*ticks > 0 && !fifo_empty(&s_state.transfer_fifo)) {
    u16 value = fifo_pop(&s_state.transfer_fifo);
    memcpy(&s_ram[s_state.transfer_address], &value, sizeof(u16));
    s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & SPU_RAM_MASK;
    *ticks -= TRANSFER_TICKS_PER_HALFWORD;
    if (is_ram_irq_triggerable() && check_ram_irq(s_state.transfer_address))
      trigger_ram_irq();
  }
}

 /* Drains as many bytes as `ticks` allow (caller supplies INT32_MAX to
 * fully flush the FIFO). */
static void execute_transfer(tick_count_t ticks)
{
  const ram_transfer_mode_t mode = (ram_transfer_mode_t)s_state.SPUCNT.b.ram_transfer_mode;
  if (mode == RAM_TRANSFER_STOPPED) {
    s_state.SPUSTAT.b.transfer_busy = 0;
    s_state.transfer_event_active = false;
    return;
  }
  internal_generate_pending_samples();

  if (mode == RAM_TRANSFER_DMA_READ) {
    while (ticks > 0 && !fifo_full(&s_state.transfer_fifo)) {
      execute_fifo_read_from_ram(&ticks);
      update_dma_request();
    }
  } else {
    while (ticks > 0 && !fifo_empty(&s_state.transfer_fifo)) {
      execute_fifo_write_to_ram(&ticks);
      update_dma_request();
    }
  }
  update_transfer_event();
}

static void manual_transfer_write(u16 value)
{
  s_spu_dbg.manual_xfer_writes++;
  if (!fifo_empty(&s_state.transfer_fifo) &&
      s_state.SPUCNT.b.ram_transfer_mode != RAM_TRANSFER_DMA_READ) {
    if (s_state.SPUCNT.b.ram_transfer_mode != RAM_TRANSFER_STOPPED)
      execute_transfer(INT32_MAX);
  }
  memcpy(&s_ram[s_state.transfer_address], &value, sizeof(u16));
  s_state.transfer_address = (s_state.transfer_address + sizeof(u16)) & SPU_RAM_MASK;
  if (is_ram_irq_triggerable() && check_ram_irq(s_state.transfer_address))
    trigger_ram_irq();
}

static void update_transfer_event(void)
{
  const ram_transfer_mode_t mode = (ram_transfer_mode_t)s_state.SPUCNT.b.ram_transfer_mode;
  if (mode == RAM_TRANSFER_STOPPED) {
    s_state.transfer_event_active = false;
  } else if (mode == RAM_TRANSFER_DMA_READ) {
    s_state.transfer_event_active = !fifo_full(&s_state.transfer_fifo);
  } else {
    s_state.transfer_event_active = !fifo_empty(&s_state.transfer_fifo);
  }
  s_state.SPUSTAT.b.transfer_busy = s_state.transfer_event_active ? 1 : 0;
}

static void update_dma_request(void)
{
  switch (s_state.SPUCNT.b.ram_transfer_mode) {
    case RAM_TRANSFER_DMA_READ:
      s_state.SPUSTAT.b.dma_read_request  = fifo_full(&s_state.transfer_fifo) ? 1 : 0;
      s_state.SPUSTAT.b.dma_write_request = 0;
      s_state.SPUSTAT.b.dma_request       = s_state.SPUSTAT.b.dma_read_request;
      break;
    case RAM_TRANSFER_DMA_WRITE:
      s_state.SPUSTAT.b.dma_read_request  = 0;
      s_state.SPUSTAT.b.dma_write_request = fifo_empty(&s_state.transfer_fifo) ? 1 : 0;
      s_state.SPUSTAT.b.dma_request       = s_state.SPUSTAT.b.dma_write_request;
      break;
    default:
      s_state.SPUSTAT.b.dma_read_request  = 0;
      s_state.SPUSTAT.b.dma_write_request = 0;
      s_state.SPUSTAT.b.dma_request       = 0;
      break;
  }
  dma_set_request(SPU_DMA_CHANNEL_INDEX, s_state.SPUSTAT.b.dma_request != 0);
}

void spu_dma_read(u32* words, u32 word_count)
{
  s_spu_dbg.dma_read_blocks++;
  s_spu_dbg.dma_read_halfwords += (u64)word_count * 2u;
  u16* halfwords = (u16*)words;
  const u32 halfword_count = word_count * 2u;
  const u32 size = fifo_size(&s_state.transfer_fifo);
  if (word_count > size) {
    u16 fill_value = 0;
    if (size > 0) {
      fifo_pop_range(&s_state.transfer_fifo, halfwords, size);
      fill_value = halfwords[size - 1];
    }
    WARNING_LOG("Transfer FIFO underflow, filling with 0x%04X", fill_value);
    for (u32 i = size; i < halfword_count; i++)
      halfwords[i] = fill_value;
  } else {
    fifo_pop_range(&s_state.transfer_fifo, halfwords, halfword_count);
  }
  update_dma_request();
  update_transfer_event();
}

void spu_dma_write(const u32* words, u32 word_count)
{
  s_spu_dbg.dma_write_blocks++;
  s_spu_dbg.dma_write_halfwords += (u64)word_count * 2u;
  const u16* halfwords = (const u16*)words;
  const u32 halfword_count = word_count * 2u;
  const u32 space = fifo_space(&s_state.transfer_fifo);
  const u32 to_transfer = (space < halfword_count) ? space : halfword_count;
  fifo_push_range(&s_state.transfer_fifo, halfwords, to_transfer);
  if (to_transfer != halfword_count)
    WARNING_LOG("Transfer FIFO overflow, dropping %u halfwords", halfword_count - to_transfer);
  update_dma_request();
  update_transfer_event();
}

static void voice_key_on(spu_voice_t* v)
{
  s_spu_dbg.voice_key_on_calls++;
  v->current_address = v->regs.adpcm_start_address & ~(u16)1;
  v->counter.bits = 0;
  v->regs.adsr_volume = 0;
  v->adpcm_last_samples[0] = 0;
  v->adpcm_last_samples[1] = 0;
  /* Zero the last-3 slots of the array.  voice_decode_block's first call
   * slides current_block_samples[28..30] into [0..2] before reading the
   * Gaussian history; zeroing the source slots here makes the post-slide
   * history exactly 0.  Zeroing [0..2] directly (the previous cupid-ps1
   * attempt at the Breath of Fire III click fix) was overwritten by that
   * very slide on the first decode and silently no-op'd.
   */
  v->current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK + 0] = 0;
  v->current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK + 1] = 0;
  v->current_block_samples[NUM_SAMPLES_PER_ADPCM_BLOCK + 2] = 0;
  v->has_samples = false;
  v->is_first_block = true;
  v->ignore_loop_address = false;
  v->adsr_phase = ADSR_PHASE_ATTACK;
  voice_update_adsr_envelope(v);
}

static void voice_key_off(spu_voice_t* v)
{
  s_spu_dbg.voice_key_off_calls++;
  if (v->adsr_phase == ADSR_PHASE_OFF || v->adsr_phase == ADSR_PHASE_RELEASE)
    return;
  v->adsr_phase = ADSR_PHASE_RELEASE;
  voice_update_adsr_envelope(v);
}

static void voice_force_off(spu_voice_t* v)
{
  if (v->adsr_phase == ADSR_PHASE_OFF)
    return;
  v->regs.adsr_volume = 0;
  v->adsr_phase = ADSR_PHASE_OFF;
}

static adsr_phase_t get_next_adsr_phase(adsr_phase_t phase)
{
  switch (phase) {
    case ADSR_PHASE_ATTACK:  return ADSR_PHASE_DECAY;
    case ADSR_PHASE_DECAY:   return ADSR_PHASE_SUSTAIN;
    case ADSR_PHASE_SUSTAIN: return ADSR_PHASE_SUSTAIN;
    case ADSR_PHASE_RELEASE:
    default:                 return ADSR_PHASE_OFF;
  }
}

static void volume_envelope_reset(volume_envelope_t* e, u8 rate, u8 rate_mask,
                                  bool decreasing, bool exponential, bool phase_invert)
{
  e->rate = rate;
  e->decreasing = decreasing;
  e->exponential = exponential;
  /* psx-spx: phase_invert is ignored when both decrease+exponential are set. */
  e->phase_invert = phase_invert && !(decreasing && exponential);
  e->counter = 0;
  e->counter_increment = 0x8000;

  /* Bitwise NOT remaps {+7,+6,+5,+4} -> {-8,-7,-6,-5} per psx-spx so the
   * sign of `step` matches the actual envelope direction even after the
   * decreasing+exponential sign-flip games. */
  s16 base_step = (s16)(7 - (rate & 3));
  e->step = ((decreasing ^ phase_invert) | (decreasing && exponential)) ? (s16)~base_step : base_step;

  if (rate < 44) {
    /* AdsrStep = StepValue SHL Max(0, 11 - ShiftValue) */
    e->step = (s16)(e->step << (11 - (rate >> 2)));
  } else if (rate >= 48) {
    /* AdsrCycles = 1 SHL Max(0, ShiftValue - 11) */
    e->counter_increment >>= ((rate >> 2) - 11);
    /* All-ones rate is the special "never tick" case for decay/release. */
    if ((rate & rate_mask) != rate_mask) {
      if (e->counter_increment < 1)
        e->counter_increment = 1;
    }
  }
}

static bool volume_envelope_tick(volume_envelope_t* e, s16* current_level)
{
  u32 this_increment = e->counter_increment;
  s32 this_step = e->step;
  if (e->exponential) {
    if (e->decreasing) {
      this_step = (this_step * (s32)*current_level) >> 15;
    } else {
      if (*current_level >= 0x6000) {
        if (e->rate < 40) {
          this_step >>= 2;
        } else if (e->rate >= 44) {
          this_increment >>= 2;
        } else {
          this_step >>= 1;
          this_increment >>= 1;
        }
      }
    }
  }

  e->counter += this_increment;
  /* MSB=1 acts as the rate gate (rate 0x76 behaves like 0x6A). */
  if (!(e->counter & 0x8000u))
    return true;
  e->counter = 0;

  s32 new_level = (s32)*current_level + this_step;
  if (!e->decreasing) {
    new_level = clamp_s32(new_level, VOLUME_ENVELOPE_MIN_VOLUME, VOLUME_ENVELOPE_MAX_VOLUME);
    *current_level = (s16)new_level;
    return new_level != ((this_step < 0) ? VOLUME_ENVELOPE_MIN_VOLUME : VOLUME_ENVELOPE_MAX_VOLUME);
  } else {
    if (e->phase_invert)
      new_level = clamp_s32(new_level, VOLUME_ENVELOPE_MIN_VOLUME, 0);
    else
      new_level = (new_level > 0) ? new_level : 0;
    *current_level = (s16)new_level;
    return new_level == 0;
  }
}

static void volume_sweep_reset(volume_sweep_t* s, volume_register_t reg)
{
  if (!reg.common.sweep_mode) {
    s->current_level = (s16)(reg.fixed.fixed_volume_shr1 * 2);
    s->envelope_active = false;
    return;
  }
  volume_envelope_reset(&s->envelope, (u8)reg.sweep.sweep_rate, 0x7F,
                        reg.sweep.sweep_direction_decrease,
                        reg.sweep.sweep_exponential, 
                        reg.sweep.sweep_phase_negative);
  s->envelope_active = (s->envelope.counter_increment > 0);
}

static void volume_sweep_tick(volume_sweep_t* s)
{
  if (!s->envelope_active)
    return;
  s->envelope_active = volume_envelope_tick(&s->envelope, &s->current_level);
}

static void voice_update_adsr_envelope(spu_voice_t* v)
{
  switch (v->adsr_phase) {
    case ADSR_PHASE_OFF:
      v->adsr_target = 0;
      volume_envelope_reset(&v->adsr_envelope, 0, 0, false, false, false);
      return;
    case ADSR_PHASE_ATTACK:
      v->adsr_target = 32767;
      volume_envelope_reset(&v->adsr_envelope, (u8)v->regs.adsr.b.attack_rate, 0x7F,
                            false, v->regs.adsr.b.attack_exponential, false);
      break;
    case ADSR_PHASE_DECAY: {
      const s32 target = (s32)((v->regs.adsr.b.sustain_level + 1u) * 0x800u);
      v->adsr_target = (s16)((target > VOLUME_ENVELOPE_MAX_VOLUME) ? VOLUME_ENVELOPE_MAX_VOLUME : target);
      volume_envelope_reset(&v->adsr_envelope, (u8)(v->regs.adsr.b.decay_rate_shr2 << 2),
                            (u8)(0x1F << 2), true, true, false);
      break;
    }
    case ADSR_PHASE_SUSTAIN:
      v->adsr_target = 0;
      volume_envelope_reset(&v->adsr_envelope, (u8)v->regs.adsr.b.sustain_rate, 0x7F,
                            v->regs.adsr.b.sustain_direction_decrease,
                            v->regs.adsr.b.sustain_exponential, false);
      break;
    case ADSR_PHASE_RELEASE:
      v->adsr_target = 0;
      volume_envelope_reset(&v->adsr_envelope, (u8)(v->regs.adsr.b.release_rate_shr2 << 2),
                            (u8)(0x1F << 2), true, v->regs.adsr.b.release_exponential, false);
      break;
    default:
      break;
  }
}

static void voice_tick_adsr(spu_voice_t* v)
{
  if (v->adsr_envelope.counter_increment > 0)
    volume_envelope_tick(&v->adsr_envelope, &v->regs.adsr_volume);

  if (v->adsr_phase != ADSR_PHASE_SUSTAIN) {
    const bool reached_target = v->adsr_envelope.decreasing
      ? (v->regs.adsr_volume <= v->adsr_target)
      : (v->regs.adsr_volume >= v->adsr_target);
    if (reached_target) {
      v->adsr_phase = get_next_adsr_phase(v->adsr_phase);
      voice_update_adsr_envelope(v);
    }
  }
}

static void voice_decode_block(spu_voice_t* v, const adpcm_block_t* block)
{
  /* Filter table: 4-bit ADPCM uses prediction filters from psx-spx. */
  static const s8 filter_table_pos[16] = {0, 60, 115, 98, 122, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const s8 filter_table_neg[16] = {0,  0, -52,-55, -60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  /* Carry over the last 3 samples for the 4-tap Gaussian interpolator. */
  v->current_block_samples[2] =
    v->current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 1];
  v->current_block_samples[1] =
    v->current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 2];
  v->current_block_samples[0] =
    v->current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + NUM_SAMPLES_PER_ADPCM_BLOCK - 3];

  const u8  shift        = adpcm_block_get_shift(block);
  const u8  filter_index = adpcm_block_get_filter(block);
  const s32 filter_pos   = filter_table_pos[filter_index];
  const s32 filter_neg   = filter_table_neg[filter_index];
  s16 last_samples[2] = { v->adpcm_last_samples[0], v->adpcm_last_samples[1] };

  for (u32 i = 0; i < NUM_SAMPLES_PER_ADPCM_BLOCK; i++) {
    /* Sign-extend the 4-bit nibble to 16 bits, scale by header shift, then
     * mix in the last two decoded samples through the prediction filter. */
    s32 sample = (s32)((s16)((u16)adpcm_block_get_nibble(block, i) << 12) >> shift);
    sample += (last_samples[0] * filter_pos) >> 6;
    sample += (last_samples[1] * filter_neg) >> 6;
    last_samples[1] = last_samples[0];
    v->current_block_samples[NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + i] =
      last_samples[0] = (s16)clamp16(sample);
  }

  v->adpcm_last_samples[0] = last_samples[0];
  v->adpcm_last_samples[1] = last_samples[1];
  v->current_block_flags.bits = block->flags.bits;
}

/* 4-tap Gaussian interpolation table (psx-spx, 0x200 entries). */
static const s16 gauss_table[0x200] = {
  -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
  -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001, -0x001,
   0x000,  0x000,  0x000,  0x000,  0x000,  0x000,  0x000,  0x001,
   0x001,  0x001,  0x001,  0x002,  0x002,  0x002,  0x003,  0x003,
   0x003,  0x004,  0x004,  0x005,  0x005,  0x006,  0x007,  0x007,
   0x008,  0x009,  0x009,  0x00A,  0x00B,  0x00C,  0x00D,  0x00E,
   0x00F,  0x010,  0x011,  0x012,  0x013,  0x015,  0x016,  0x018,
   0x019,  0x01B,  0x01C,  0x01E,  0x020,  0x021,  0x023,  0x025,
   0x027,  0x029,  0x02C,  0x02E,  0x030,  0x033,  0x035,  0x038,
   0x03A,  0x03D,  0x040,  0x043,  0x046,  0x049,  0x04D,  0x050,
   0x054,  0x057,  0x05B,  0x05F,  0x063,  0x067,  0x06B,  0x06F,
   0x074,  0x078,  0x07D,  0x082,  0x087,  0x08C,  0x091,  0x096,
   0x09C,  0x0A1,  0x0A7,  0x0AD,  0x0B3,  0x0BA,  0x0C0,  0x0C7,
   0x0CD,  0x0D4,  0x0DB,  0x0E3,  0x0EA,  0x0F2,  0x0FA,  0x101,
   0x10A,  0x112,  0x11B,  0x123,  0x12C,  0x135,  0x13F,  0x148,
   0x152,  0x15C,  0x166,  0x171,  0x17B,  0x186,  0x191,  0x19C,
   0x1A8,  0x1B4,  0x1C0,  0x1CC,  0x1D9,  0x1E5,  0x1F2,  0x200,
   0x20D,  0x21B,  0x229,  0x237,  0x246,  0x255,  0x264,  0x273,
   0x283,  0x293,  0x2A3,  0x2B4,  0x2C4,  0x2D6,  0x2E7,  0x2F9,
   0x30B,  0x31D,  0x330,  0x343,  0x356,  0x36A,  0x37E,  0x392,
   0x3A7,  0x3BC,  0x3D1,  0x3E7,  0x3FC,  0x413,  0x42A,  0x441,
   0x458,  0x470,  0x488,  0x4A0,  0x4B9,  0x4D2,  0x4EC,  0x506,
   0x520,  0x53B,  0x556,  0x572,  0x58E,  0x5AA,  0x5C7,  0x5E4,
   0x601,  0x61F,  0x63E,  0x65C,  0x67C,  0x69B,  0x6BB,  0x6DC,
   0x6FD,  0x71E,  0x740,  0x762,  0x784,  0x7A7,  0x7CB,  0x7EF,
   0x813,  0x838,  0x85D,  0x883,  0x8A9,  0x8D0,  0x8F7,  0x91E,
   0x946,  0x96F,  0x998,  0x9C1,  0x9EB,  0xA16,  0xA40,  0xA6C,
   0xA98,  0xAC4,  0xAF1,  0xB1E,  0xB4C,  0xB7A,  0xBA9,  0xBD8,
   0xC07,  0xC38,  0xC68,  0xC99,  0xCCB,  0xCFD,  0xD30,  0xD63,
   0xD97,  0xDCB,  0xE00,  0xE35,  0xE6B,  0xEA1,  0xED7,  0xF0F,
   0xF46,  0xF7F,  0xFB7,  0xFF1, 0x102A, 0x1065, 0x109F, 0x10DB,
  0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
  0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
  0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
  0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
  0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
  0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
  0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
  0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
  0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
  0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
  0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
  0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
  0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
  0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
  0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
  0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
  0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
  0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
  0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
  0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
  0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
  0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
  0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
  0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
  0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
  0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
  0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
  0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
  0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
  0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
  0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
  0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
  0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3, 
};

static s32 voice_interpolate(const spu_voice_t* v)
{
  const u8  i = (u8)v->counter.b.interpolation_index;
  const u32 s = NUM_SAMPLES_FROM_LAST_ADPCM_BLOCK + ZeroExtend32(v->counter.b.sample_index);

  s32 out  = (s32)gauss_table[0x0FF - i] * (s32)v->current_block_samples[s - 3];
  out     += (s32)gauss_table[0x1FF - i] * (s32)v->current_block_samples[s - 2];
  out     += (s32)gauss_table[0x100 + i] * (s32)v->current_block_samples[s - 1];
  out     += (s32)gauss_table[0x000 + i] * (s32)v->current_block_samples[s - 0];
  return out >> 15;
}

static void read_adpcm_block(u16 address, adpcm_block_t* block)
{
  u32 ram_address = (ZeroExtend32(address) * 8u) & SPU_RAM_MASK;
  if (is_ram_irq_triggerable() &&
      (check_ram_irq(ram_address) || check_ram_irq((ram_address + 8u) & SPU_RAM_MASK))) {
    trigger_ram_irq();
  }

  if ((ram_address + sizeof(adpcm_block_t)) <= SPU_RAM_SIZE) {
    memcpy(block, &s_ram[ram_address], sizeof(adpcm_block_t));
    return;
  }
  /* Slow path: header crosses the 512KB wrap. */
  block->shift_filter.bits = s_ram[ram_address];
  ram_address = (ram_address + 1u) & SPU_RAM_MASK;
  block->flags.bits = s_ram[ram_address];
  ram_address = (ram_address + 1u) & SPU_RAM_MASK;
  for (u32 i = 0; i < 14; i++) {
    block->data[i] = s_ram[ram_address];
    ram_address = (ram_address + 1u) & SPU_RAM_MASK;
  }
}

static void sample_voice(u32 voice_index, s32* out_left, s32* out_right)
{
  spu_voice_t* voice = &s_state.voices[voice_index];
  if (!voice_is_on(voice) && !s_state.SPUCNT.b.irq9_enable) {
    voice->last_volume = 0;
    *out_left = 0;
    *out_right = 0;
    return;
  }

  if (!voice->has_samples) {
    adpcm_block_t block;
    read_adpcm_block(voice->current_address, &block);
    voice_decode_block(voice, &block);
    voice->has_samples = true;
    if (voice->current_block_flags.b.loop_start && !voice->ignore_loop_address)
      voice->regs.adpcm_repeat_address = voice->current_address;
  }

  s32 volume;
  if (voice->regs.adsr_volume != 0) {
     s32 sample = is_voice_noise_enabled(voice_index)
      ? (s32)get_voice_noise_level() 
      : voice_interpolate(voice);
    volume = apply_volume(sample, voice->regs.adsr_volume);
  } else {
    volume = 0;
  }

  voice->last_volume = volume;
  if (voice->adsr_phase != ADSR_PHASE_OFF)
    voice_tick_adsr(voice);

  /* Pitch modulation: scale this voice's pitch by the previous voice's
   * post-ADSR sample.  Voice 0 cannot be PM'd. */
  u16 step = voice->regs.adpcm_sample_rate;
  if (is_pitch_modulation_enabled(voice_index)) {
    const s32 last = clamp_s32(s_state.voices[voice_index - 1].last_volume, -0x8000, 0x7FFF) + 0x8000;
    step = Truncate16((u32)((SignExtend32(step) * last) >> 15));
  }
  if (step > 0x3FFFu)
    step = 0x3FFFu;

  voice->counter.bits += step;

  if (voice->counter.b.sample_index >= NUM_SAMPLES_PER_ADPCM_BLOCK) {
    voice->counter.b.sample_index -= NUM_SAMPLES_PER_ADPCM_BLOCK;
    voice->has_samples = false;
    voice->is_first_block = false;
    voice->current_address += 2;

    if (voice->current_block_flags.b.loop_end) {
      s_state.endx_register |= (1u << voice_index);
      voice->current_address = voice->regs.adpcm_repeat_address & ~(u16)1;
      if (!voice->current_block_flags.b.loop_repeat) {
        /* End+Mute: ignored when noise is enabled (ADPCM still decodes). */
        if (!is_voice_noise_enabled(voice_index))
          voice_force_off(voice);
      }
    }
  }

  *out_left  = apply_volume(volume, voice->left_volume.current_level);
  *out_right = apply_volume(volume, voice->right_volume.current_level);
  volume_sweep_tick(&voice->left_volume);
  volume_sweep_tick(&voice->right_volume);
}

static void update_noise(void)
{
  /* Noise waveform from Dr Hell via pcsx-r; matches PS1 hardware. */
  static const u8 noise_wave_add[64] = {
    1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
    1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
    0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1,
    0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 
  };
  static const u8 noise_freq_add[5] = {0, 84, 140, 180, 210};

  const u32 noise_clock = s_state.SPUCNT.b.noise_clock;
  const u32 level = (0x8000u >> (noise_clock >> 2)) << 16;

  s_state.noise_count += 0x10000u + noise_freq_add[noise_clock & 3u];
  if ((s_state.noise_count & 0xFFFFu) >= noise_freq_add[4]) {
    s_state.noise_count += 0x10000u;
    s_state.noise_count -= noise_freq_add[noise_clock & 3u];
  }
  if (s_state.noise_count < level)
    return;
  s_state.noise_count %= level;
  s_state.noise_level = (s_state.noise_level << 1) | noise_wave_add[(s_state.noise_level >> 10) & 63u];
}

static u32 reverb_memory_address(u32 address)
{
  /* Keep the address inside the reverb work area: when the linear offset
   * runs off the end (high bit of `offset << 13`), the base address is
   * added back in to fold it.  Mask with the same MASK afterwards. */
  static const u32 MASK = (SPU_RAM_SIZE - 1u) / 2u;
  u32 offset = s_state.reverb_current_address + (address & MASK);
  offset += s_state.reverb_base_address & ((u32)((s32)(offset << 13) >> 31));
  return (offset & MASK) * 2u;
}

static s16 reverb_read(u32 address, s32 offset)
{
  const u32 real_address = reverb_memory_address((address << 2) + (u32)offset);
  s16 data;
  memcpy(&data, &s_ram[real_address], sizeof(data));
  return data;
}

static void reverb_write(u32 address, s16 data)
{
  const u32 real_address = reverb_memory_address(address << 2);
  memcpy(&s_ram[real_address], &data, sizeof(data));
}

/* 39-tap FIR with the centre tap (0x4000) folded in separately; the table */
static const s32 resample_coeff[20] = {
  -0x0001,  0x0002, -0x000A,  0x0023, -0x0067,  0x010A, -0x0268,  0x0534,
  -0x0B90,  0x2806,  0x2806, -0x0B90,  0x0534, -0x0268,  0x010A, -0x0067,
   0x0023, -0x000A,  0x0002, -0x0001 
};

ALWAYS_INLINE static s32 reverb_iiasm(s16 insamp)
{
  /* `IIR_ALPHA == -32768` is a hardware special case: would-be overflow
   * collapses to 0 when `insamp == -32768`, otherwise to insamp * -65536. */
  if (s_state.reverb_registers.IIR_ALPHA == -32768)
    return (insamp == -32768) ? 0 : ((s32)insamp * -65536);
  return (s32)insamp * (32768 - (s32)s_state.reverb_registers.IIR_ALPHA);
}
ALWAYS_INLINE static s32 reverb_neg(s32 v)
{
  /* `-(-32768)` would wrap; saturate instead. */
  return (v == -32768) ? 0x7FFF : -v;
}

static void process_reverb(s32 left_in, s32 right_in, s32* left_out, s32* right_out)
{
  s_state.last_reverb_input[0] = (s16)left_in;
  s_state.last_reverb_input[1] = (s16)right_in;

  /* Resampling buffer is duplicated to avoid having to manually wrap
   * the index in the inner FIR loop. */
  const s32 pos = s_state.reverb_resample_buffer_position;
  s_state.reverb_downsample_buffer[0][pos | 0x00] =
    s_state.reverb_downsample_buffer[0][pos | 0x40] = (s16)left_in;
  s_state.reverb_downsample_buffer[1][pos | 0x00] =
    s_state.reverb_downsample_buffer[1][pos | 0x40] = (s16)right_in;

  s32 out[2];
  if (pos & 1) {
    s32 downsampled[2];
    for (u32 channel = 0; channel < 2; channel++) {
      const s16* src = &s_state.reverb_downsample_buffer[channel][(pos - 38) & 0x3F];
      /* 19 paired multiplies + a centre tap (0x4000 * src[19]) >> 15.
       * Index pattern matches the SIMD layout: 0..3, 8..11, 16..19,
       * 24..27, 32..35 -> coeff[0..3,4..7,8..11,12..15,16..19]. */
      s64 acc = 0;
      acc += (s64)resample_coeff[0]  * (s32)src[0];
      acc += (s64)resample_coeff[1]  * (s32)src[1];
      acc += (s64)resample_coeff[2]  * (s32)src[2];
      acc += (s64)resample_coeff[3]  * (s32)src[3];
      acc += (s64)resample_coeff[4]  * (s32)src[8];
      acc += (s64)resample_coeff[5]  * (s32)src[9];
      acc += (s64)resample_coeff[6]  * (s32)src[10];
      acc += (s64)resample_coeff[7]  * (s32)src[11];
      acc += (s64)resample_coeff[8]  * (s32)src[16];
      acc += (s64)resample_coeff[9]  * (s32)src[17];
      acc += (s64)resample_coeff[10] * (s32)src[18];
      acc += (s64)resample_coeff[11] * (s32)src[19];
      acc += (s64)resample_coeff[12] * (s32)src[24];
      acc += (s64)resample_coeff[13] * (s32)src[25];
      acc += (s64)resample_coeff[14] * (s32)src[26];
      acc += (s64)resample_coeff[15] * (s32)src[27];
      acc += (s64)resample_coeff[16] * (s32)src[32];
      acc += (s64)resample_coeff[17] * (s32)src[33];
      acc += (s64)resample_coeff[18] * (s32)src[34];
      acc += (s64)resample_coeff[19] * (s32)src[35];
      downsampled[channel] = clamp16((s32)((acc + (s64)0x4000 * (s32)src[19]) >> 15));
    }

    for (u32 channel = 0; channel < 2; channel++) {
      if (s_state.SPUCNT.b.reverb_master_enable) {
        const s32 IIR_INPUT_A = clamp16(
          (((reverb_read(s_state.reverb_registers.IIR_SRC_A[channel ^ 0], 0) *
              s_state.reverb_registers.IIR_COEF) >> 14) + 
           ((downsampled[channel] * s_state.reverb_registers.IN_COEF[channel]) >> 14)) >> 1);
        const s32 IIR_INPUT_B = clamp16(
          (((reverb_read(s_state.reverb_registers.IIR_SRC_B[channel ^ 1], 0) *
              s_state.reverb_registers.IIR_COEF) >> 14) + 
           ((downsampled[channel] * s_state.reverb_registers.IN_COEF[channel]) >> 14)) >> 1);

        const s32 IIR_A = clamp16(
          (((IIR_INPUT_A * s_state.reverb_registers.IIR_ALPHA) >> 14) +
           (reverb_iiasm(reverb_read(s_state.reverb_registers.IIR_DEST_A[channel], -1)) >> 14)) >> 1);
        const s32 IIR_B = clamp16(
          (((IIR_INPUT_B * s_state.reverb_registers.IIR_ALPHA) >> 14) +
           (reverb_iiasm(reverb_read(s_state.reverb_registers.IIR_DEST_B[channel], -1)) >> 14)) >> 1);

        reverb_write(s_state.reverb_registers.IIR_DEST_A[channel], (s16)IIR_A);
        reverb_write(s_state.reverb_registers.IIR_DEST_B[channel], (s16)IIR_B);
      }

      const s32 ACC =
        ((reverb_read(s_state.reverb_registers.ACC_SRC_A[channel], 0) * s_state.reverb_registers.ACC_COEF_A) >> 14) +
        ((reverb_read(s_state.reverb_registers.ACC_SRC_B[channel], 0) * s_state.reverb_registers.ACC_COEF_B) >> 14) +
        ((reverb_read(s_state.reverb_registers.ACC_SRC_C[channel], 0) * s_state.reverb_registers.ACC_COEF_C) >> 14) + 
        ((reverb_read(s_state.reverb_registers.ACC_SRC_D[channel], 0) * s_state.reverb_registers.ACC_COEF_D) >> 14);

      const s32 FB_A = reverb_read((u32)(s_state.reverb_registers.MIX_DEST_A[channel] -
                                          s_state.reverb_registers.FB_SRC_A), 0);
      const s32 FB_B = reverb_read((u32)(s_state.reverb_registers.MIX_DEST_B[channel] -
                                          s_state.reverb_registers.FB_SRC_B), 0);
      const s32 MDA = clamp16((ACC + ((FB_A * reverb_neg(s_state.reverb_registers.FB_ALPHA)) >> 14)) >> 1);
      const s32 MDB = clamp16(FB_A + ((((MDA * s_state.reverb_registers.FB_ALPHA) >> 14) +
                                       ((FB_B * reverb_neg(s_state.reverb_registers.FB_X)) >> 14)) >> 1));

      const s16 upsamp = (s16)clamp16(FB_B + ((MDB * s_state.reverb_registers.FB_X) >> 15));
      s_state.reverb_upsample_buffer[channel][(pos >> 1) | 0x20] =
        s_state.reverb_upsample_buffer[channel][pos >> 1] = upsamp;

      if (s_state.SPUCNT.b.reverb_master_enable) {
        reverb_write(s_state.reverb_registers.MIX_DEST_A[channel], (s16)MDA);
        reverb_write(s_state.reverb_registers.MIX_DEST_B[channel], (s16)MDB);
      }
    }

    s_state.reverb_current_address = (s_state.reverb_current_address + 1u) & 0x3FFFFu;
    if (s_state.reverb_current_address == 0)
      s_state.reverb_current_address = s_state.reverb_base_address;

    /* Upsample (39-tap FIR back to 44.1kHz). */
    for (u32 channel = 0; channel < 2; channel++) {
      const s16* src = &s_state.reverb_upsample_buffer[channel][((pos >> 1) - 19) & 0x1F];
      s64 acc = 0;
      acc += (s64)resample_coeff[0]  * (s32)src[0];
      acc += (s64)resample_coeff[1]  * (s32)src[1];
      acc += (s64)resample_coeff[2]  * (s32)src[2];
      acc += (s64)resample_coeff[3]  * (s32)src[3];
      acc += (s64)resample_coeff[4]  * (s32)src[4];
      acc += (s64)resample_coeff[5]  * (s32)src[5];
      acc += (s64)resample_coeff[6]  * (s32)src[6];
      acc += (s64)resample_coeff[7]  * (s32)src[7];
      acc += (s64)resample_coeff[8]  * (s32)src[8];
      acc += (s64)resample_coeff[9]  * (s32)src[9];
      acc += (s64)resample_coeff[10] * (s32)src[10];
      acc += (s64)resample_coeff[11] * (s32)src[11];
      acc += (s64)resample_coeff[12] * (s32)src[12];
      acc += (s64)resample_coeff[13] * (s32)src[13];
      acc += (s64)resample_coeff[14] * (s32)src[14];
      acc += (s64)resample_coeff[15] * (s32)src[15];
      acc += (s64)resample_coeff[16] * (s32)src[16];
      acc += (s64)resample_coeff[17] * (s32)src[17];
      acc += (s64)resample_coeff[18] * (s32)src[18];
      acc += (s64)resample_coeff[19] * (s32)src[19];
      out[channel] = clamp_s32((s32)(acc >> 14), -32768, 32767);
    }
  } else {
    /* Even ticks reuse the held upsample sample (output runs at 22.05kHz). */
    const u32 idx = (u32)(((pos >> 1) - 19) & 0x1F) + 9u;
    out[0] = s_state.reverb_upsample_buffer[0][idx];
    out[1] = s_state.reverb_upsample_buffer[1][idx];
  }

  s_state.reverb_resample_buffer_position = (pos + 1) & 0x3F;

  s_state.last_reverb_output[0] = *left_out  = apply_volume(out[0], s_state.reverb_registers.vLOUT);
  s_state.last_reverb_output[1] = *right_out = apply_volume(out[1], s_state.reverb_registers.vROUT);
}

static void generate_one_frame(bool first_in_batch)
{
  s32 left_sum = 0;
  s32 right_sum = 0;
  s32 reverb_in_left = 0;
  s32 reverb_in_right = 0;

  u32 reverb_on_register = s_state.reverb_on_register;

  for (u32 voice = 0; voice < NUM_VOICES; voice++) {
    s32 vleft, vright;
    sample_voice(voice, &vleft, &vright);
    left_sum  += vleft;
    right_sum += vright;
    if (reverb_on_register & 1u) {
      reverb_in_left  += vleft;
      reverb_in_right += vright;
    }
    reverb_on_register >>= 1;
  }

  if (!s_state.SPUCNT.b.mute_n) {
    left_sum = right_sum = 0;
    reverb_in_left = reverb_in_right = 0;
  }

  update_noise();

  /* CDDA mix-in. */
  s16 cd_left = 0, cd_right = 0;
  cdrom_get_audio_frame(&cd_left, &cd_right);
  if (s_state.SPUCNT.b.cd_audio_enable) {
    const s32 cdv_left  = apply_volume((s32)cd_left,  s_state.cd_audio_volume_left);
    const s32 cdv_right = apply_volume((s32)cd_right, s_state.cd_audio_volume_right);
    left_sum  += cdv_left;
    right_sum += cdv_right;
    if (s_state.SPUCNT.b.cd_audio_reverb) {
      reverb_in_left  += cdv_left;
      reverb_in_right += cdv_right;
    }
  }

  s32 reverb_out_left, reverb_out_right;
  process_reverb(clamp16(reverb_in_left), clamp16(reverb_in_right),
                 &reverb_out_left, &reverb_out_right);

  left_sum  += reverb_out_left;
  right_sum += reverb_out_right;

  const s16 final_left  = (s16)apply_volume(clamp16(left_sum),  s_state.main_volume_left.current_level);
  const s16 final_right = (s16)apply_volume(clamp16(right_sum), s_state.main_volume_right.current_level);

  if (!s_state.audio_output_muted) {
    s_spu_dbg.ring_push_frames++;
    if (final_left || final_right)
      s_spu_dbg.ring_push_nonzero_frames++;
    push_sample_to_stream(s_state.audio_stream, final_left, final_right);
  }
  s_spu_dbg.frames_generated++;
  /* Track peak voice-on count for triage. */
  {
    u32 on = 0;
    for (u32 i = 0; i < NUM_VOICES; i++)
      if (s_state.voices[i].adsr_phase != ADSR_PHASE_OFF) on++;
    if (on > s_spu_dbg.max_voices_on_seen) s_spu_dbg.max_voices_on_seen = on;
  }

  volume_sweep_tick(&s_state.main_volume_left);
  volume_sweep_tick(&s_state.main_volume_right);

  /* Capture buffers (4 x 1KB regions, raw CDDA + voice 1/3 dumps). */
  write_to_capture_buffer(0, cd_left);
  write_to_capture_buffer(1, cd_right);
  write_to_capture_buffer(2, (s16)clamp16(s_state.voices[1].last_volume));
  write_to_capture_buffer(3, (s16)clamp16(s_state.voices[3].last_volume));
  increment_capture_buffer_position();

  if (first_in_batch && (s_state.key_off_register != 0 || s_state.key_on_register != 0)) {
    u32 key_off_register = s_state.key_off_register;
    u32 key_on_register  = s_state.key_on_register;
    s_state.key_off_register = 0;
    s_state.key_on_register  = 0;
    for (u32 voice = 0; voice < NUM_VOICES; voice++) {
      if (key_off_register & 1u)
        voice_key_off(&s_state.voices[voice]);
      key_off_register >>= 1;
      if (key_on_register & 1u) {
        s_state.endx_register &= ~(1u << voice);
        voice_key_on(&s_state.voices[voice]);
      }
      key_on_register >>= 1;
    }
  }
}

void spu_execute(tick_count_t cpu_ticks)
{
  s_spu_dbg.spu_execute_calls++;
  /* Convert the requested span of CPU ticks into 44.1kHz frames.
   * Overclock path counts in (ticks * D) units with divider = N * 768;
   * otherwise plain 768-tick frames. */
  u32 remaining_frames;
  if (g_settings.cpu_overclock_active) {
    const u64 num = ((u64)(u32)cpu_ticks * (u64)g_settings.cpu_overclock_denominator)
                    + (u64)(u32)s_state.ticks_carry;
    remaining_frames    = (u32)(num / (u64)(u32)s_state.cpu_tick_divider);
    s_state.ticks_carry = (tick_count_t)(num % (u64)(u32)s_state.cpu_tick_divider);
  } else {
    const tick_count_t total = cpu_ticks + s_state.ticks_carry;
    remaining_frames    = (u32)(total / SYSCLK_TICKS_PER_SPU_TICK);
    s_state.ticks_carry = total % SYSCLK_TICKS_PER_SPU_TICK;
  }

  /* Also advance any in-flight transfer (the C tree does not have a
   * second timing event, so the transfer FIFO progresses inline). */
  if (s_state.transfer_event_active) {
    s_state.transfer_ticks_carry += cpu_ticks;
    while (s_state.transfer_ticks_carry >= TRANSFER_TICKS_PER_HALFWORD &&
           s_state.transfer_event_active) {
      tick_count_t budget = s_state.transfer_ticks_carry;
      execute_transfer(budget);
      /* execute_transfer drains as much as it can; subtract what it ate. */
      s_state.transfer_ticks_carry = budget;
      if (!s_state.transfer_event_active)
        break;
    }
  }

  bool first = true;
  while (remaining_frames > 0) {
    generate_one_frame(first);
    first = false;
    remaining_frames--;
  }
}

void spu_generate_pending_samples(void)
{
  /* Force the SPU tick event to fire NOW, draining all CPU ticks elapsed
   * GeneratePendingSamples -> tick_event.InvokeEarly().  Without this, the
   * audio_fifo on the CDROM side accumulates faster than SPU drains it,
   * triggering the LOW_WATERMARK skip in ProcessXAADPCMSector and
   * dropping XA-ADPCM sectors mid-FMV (RE2 intro / hospital cutscene). */
  timing_event_invoke_early(&s_state.tick_event, /*force=*/true);
}

static void internal_generate_pending_samples(void)
{
  /* Carry units differ between overclock-on (ticks*D, step = N*768) and
   * overclock-off (raw ticks, step = 768).  Normally a no-op because
   * spu_execute already drained carry to < step; safety net for forced
   * drain paths (e.g. manual_transfer_write at line 1253 above). */
  const tick_count_t step = g_settings.cpu_overclock_active
                              ? s_state.cpu_tick_divider
                              : (tick_count_t)SYSCLK_TICKS_PER_SPU_TICK;
  while (s_state.ticks_carry >= step) {
    generate_one_frame(false);
    s_state.ticks_carry -= step;
  }
}

tick_count_t spu_get_cpu_ticks_until_next_event(void)
{
  const tick_count_t until = SYSCLK_TICKS_PER_SPU_TICK - s_state.ticks_carry;
  return (until > 0) ? until : 1;
}

static void update_event_interval(void)
{
  const tick_count_t period = s_state.cpu_ticks_per_spu_tick * 256;
  timing_event_set_interval_and_schedule(&s_state.tick_event, period);
}

const u8* spu_get_ram         (void) { return s_ram; }
u8*       spu_get_writable_ram(void) { return s_ram; }

bool spu_is_audio_output_muted (void)        { return s_state.audio_output_muted; }
void spu_set_audio_output_muted(bool muted)  { s_state.audio_output_muted = muted; }

void spu_set_output_stream(core_audio_stream_t* stream)
{
  /* core_audio_stream owns the source.read_frames thunk + ring; no prefill
   * is necessary; when the SDL callback fires before spu_execute() has
   * produced anything, core_audio_stream's underrun path emits silence. */
  s_state.audio_stream = stream;
}
core_audio_stream_t* spu_get_output_stream(void) { return s_state.audio_stream; }
