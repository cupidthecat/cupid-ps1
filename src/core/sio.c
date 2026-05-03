#include "core/sio.h"

#include "util/state_wrapper.h"

#include "common/bitutils.h"
#include "common/log.h"

LOG_CHANNEL(SIO);

/* Register layouts: laid out as bitfields in the original; we keep raw u16/
 * u32 storage and use shift/mask helpers for the few bits we need (only the
 * RESET bit is consumed). */
#define SIO_CTRL_RESET_BIT  (1u << 6)
/* SIO_STAT bits: TXRDY=0, TXDONE=2, DSR_INPUTLEVEL=7, CTS_INPUTLEVEL=8 */
#define SIO_STAT_TXRDY            (1u << 0)
#define SIO_STAT_TXDONE           (1u << 2)
#define SIO_STAT_DSR_INPUT_LEVEL  (1u << 7)
#define SIO_STAT_CTS_INPUT_LEVEL  (1u << 8)

/* Forward decl for the optional TTY hook; bus.c provides the real symbol
 * once it lands.  Made weak so the SIO unit links standalone. */
extern void bus_add_tty_character(char c) __attribute__((weak));

/* Settings flag is owned by the parallel settings agent; we read it via an
 * extern accessor that returns false until the settings module is wired in. */
extern bool g_sio_redirect_to_tty __attribute__((weak));

static u16 s_sio_ctrl;
static u32 s_sio_stat;
static u16 s_sio_mode;
static u16 s_sio_baud;

static void sio_soft_reset(void);

void sio_initialize(void)
{
  sio_reset();
}

void sio_shutdown(void)
{
}

void sio_reset(void)
{
  sio_soft_reset();
}

bool sio_do_state(state_wrapper_t* sw)
{
  state_wrapper_do_u16(sw, &s_sio_ctrl);
  state_wrapper_do_u32(sw, &s_sio_stat);
  state_wrapper_do_u16(sw, &s_sio_mode);
  state_wrapper_do_u16(sw, &s_sio_baud);
  return !state_wrapper_has_error(sw);
}

u32 sio_read_register(u32 offset)
{
  switch (offset)
  {
    case 0x00: /* SIO_DATA: returns 0xFF (no device attached). */
    {
      ERROR_LOG("Read SIO_DATA");
      const u8 v = 0xFF;
      return ZeroExtend32(v) | (ZeroExtend32(v) << 8) | (ZeroExtend32(v) << 16) | (ZeroExtend32(v) << 24);
    }
    case 0x04: /* SIO_STAT */
      return s_sio_stat;
    case 0x08: /* SIO_MODE */
      return ZeroExtend32(s_sio_mode);
    case 0x0A: /* SIO_CTRL */
      return ZeroExtend32(s_sio_ctrl);
    case 0x0E: /* SIO_BAUD */
      return ZeroExtend32(s_sio_baud);
    default:
      ERROR_LOG("Unknown register read: 0x%X", offset);
      return UINT32_C(0xFFFFFFFF);
  }
}

void sio_write_register(u32 offset, u32 value)
{
  switch (offset)
  {
    case 0x00: /* SIO_DATA */
      if ((&g_sio_redirect_to_tty != NULL) && g_sio_redirect_to_tty && (&bus_add_tty_character != NULL))
        bus_add_tty_character((char)value);
      else
        WARNING_LOG("SIO_DATA (W) <- 0x%02X", value);
      return;

    case 0x0A: /* SIO_CTRL */
      DEBUG_LOG("SIO_CTRL <- 0x%04X", value);
      s_sio_ctrl = Truncate16(value);
      if (s_sio_ctrl & SIO_CTRL_RESET_BIT)
        sio_soft_reset();
      return;

    case 0x08: /* SIO_MODE */
      DEBUG_LOG("SIO_MODE <- 0x%08X", value);
      s_sio_mode = Truncate16(value);
      return;

    case 0x0E: /* SIO_BAUD */
      DEBUG_LOG("SIO_BAUD <- 0x%08X", value);
      s_sio_baud = Truncate16(value);
      return;

    default:
      ERROR_LOG("Unknown register write: 0x%X <- 0x%08X", offset, value);
      return;
  }
}

static void sio_soft_reset(void)
{
  s_sio_ctrl = 0;
  /* Quiescent SIO: TXDONE/TXRDY high, DSR/CTS asserted by the (absent)
   * peer. */
  s_sio_stat = SIO_STAT_DSR_INPUT_LEVEL | SIO_STAT_CTS_INPUT_LEVEL |
               SIO_STAT_TXDONE | SIO_STAT_TXRDY;
  s_sio_mode = 0;
  s_sio_baud = 0xDC;
}
