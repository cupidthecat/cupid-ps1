/*
 *

 * fmt:: collapsed to printf-style varargs.  Each ported call site translates
 * {} → %d/%s/etc as it lands.  Public API: log_*; macros preserved verbatim
 */
#ifndef CUPID_COMMON_LOG_H
#define CUPID_COMMON_LOG_H

#include "log_channels.h"
#include "types.h"

#include <stdarg.h>

typedef enum {
  LOG_LEVEL_NONE = 0,
  LOG_LEVEL_ERROR,
  LOG_LEVEL_WARNING,
  LOG_LEVEL_INFO,
  LOG_LEVEL_VERBOSE,
  LOG_LEVEL_DEV,
  LOG_LEVEL_DEBUG,
  LOG_LEVEL_TRACE,
  LOG_LEVEL_MAX_COUNT
} log_level_t;

typedef enum {
  LOG_COLOR_DEFAULT = 0,
  LOG_COLOR_BLACK,
  LOG_COLOR_RED,
  LOG_COLOR_GREEN,
  LOG_COLOR_BLUE,
  LOG_COLOR_MAGENTA,
  LOG_COLOR_ORANGE,
  LOG_COLOR_CYAN,
  LOG_COLOR_YELLOW,
  LOG_COLOR_WHITE,
  LOG_COLOR_STRONG_BLACK,
  LOG_COLOR_STRONG_RED,
  LOG_COLOR_STRONG_GREEN,
  LOG_COLOR_STRONG_BLUE,
  LOG_COLOR_STRONG_MAGENTA,
  LOG_COLOR_STRONG_ORANGE,
  LOG_COLOR_STRONG_CYAN,
  LOG_COLOR_STRONG_YELLOW,
  LOG_COLOR_STRONG_WHITE,
  LOG_COLOR_MAX_COUNT
} log_color_t;

typedef enum {
#define LOG_CHANNEL_ENUM(X) LOG_CHANNEL_##X,
  ENUMERATE_LOG_CHANNELS(LOG_CHANNEL_ENUM)
#undef LOG_CHANNEL_ENUM
  LOG_CHANNEL_MAX_COUNT
} log_channel_t;

#define LOG_DEFAULT_LEVEL LOG_LEVEL_INFO

typedef u32 log_message_category_t;

ALWAYS_INLINE u32 log_pack_category(log_channel_t ch, log_level_t lv, log_color_t col)
{
  return (((u32)col) << 10) | (((u32)ch) << 3) | ((u32)lv);
}
ALWAYS_INLINE log_color_t   log_unpack_color  (u32 cat) { return (log_color_t)  ((cat >> 10) & 0x1Fu); }
ALWAYS_INLINE log_channel_t log_unpack_channel(u32 cat) { return (log_channel_t)((cat >>  3) & 0x7Fu); }
ALWAYS_INLINE log_level_t   log_unpack_level  (u32 cat) { return (log_level_t)  ( cat        & 0x07u); }

typedef void (*log_callback_t)(void* user, u32 cat, const char* func, const char* msg, size_t msg_len);

void log_register_callback  (log_callback_t cb, void* user);
void log_unregister_callback(log_callback_t cb, void* user);

const char* log_get_channel_name(log_channel_t ch);
log_color_t log_get_color_for_level(log_level_t lv);

float log_get_current_message_time(void);
bool  log_are_console_output_timestamps_enabled(void);

bool log_is_console_output_enabled(void);
void log_set_console_output_params(bool enabled, bool timestamps);

void log_set_file_output_params(bool enabled, const char* filename, bool timestamps);

log_level_t log_get_log_level(void);
bool log_is_log_visible(log_level_t lv, log_channel_t ch);
void log_set_log_level(log_level_t lv);
void log_set_channel_enabled(log_channel_t ch, bool enabled);

void log_write          (u32 cat,                       const char* msg, size_t msg_len);
void log_write_func     (u32 cat, const char* func,     const char* msg, size_t msg_len);
void log_write_fmt      (u32 cat,                       const char* fmt, ...) PRINTFLIKE(2, 3);
void log_write_func_fmt (u32 cat, const char* func,     const char* fmt, ...) PRINTFLIKE(3, 4);
void log_write_vfmt     (u32 cat,                       const char* fmt, va_list ap);
void log_write_func_vfmt(u32 cat, const char* func,     const char* fmt, va_list ap);

#define LOG_CHANNEL(name) static const log_channel_t __attribute__((unused)) ___LogChannel___ = LOG_CHANNEL_##name

#define GENERIC_LOG(channel, level, color, ...)                                                                        \
  do {                                                                                                                 \
    if ((level) <= log_get_log_level())                                                                                \
      log_write_fmt(log_pack_category((channel), (level), (color)), __VA_ARGS__);                                      \
  } while (0)

#define GENERIC_FUNC_LOG(channel, level, color, ...)                                                                   \
  do {                                                                                                                 \
    if ((level) <= log_get_log_level())                                                                                \
      log_write_func_fmt(log_pack_category((channel), (level), (color)), __func__, __VA_ARGS__);                       \
  } while (0)

#define ERROR_LOG(...)   GENERIC_FUNC_LOG(___LogChannel___, LOG_LEVEL_ERROR,   LOG_COLOR_DEFAULT, __VA_ARGS__)
#define WARNING_LOG(...) GENERIC_FUNC_LOG(___LogChannel___, LOG_LEVEL_WARNING, LOG_COLOR_DEFAULT, __VA_ARGS__)
#define INFO_LOG(...)    GENERIC_LOG     (___LogChannel___, LOG_LEVEL_INFO,    LOG_COLOR_DEFAULT, __VA_ARGS__)
#define VERBOSE_LOG(...) GENERIC_LOG     (___LogChannel___, LOG_LEVEL_VERBOSE, LOG_COLOR_DEFAULT, __VA_ARGS__)
#define DEV_LOG(...)     GENERIC_LOG     (___LogChannel___, LOG_LEVEL_DEV,     LOG_COLOR_DEFAULT, __VA_ARGS__)

#if !defined(NDEBUG)
#  define DEBUG_LOG(...) GENERIC_LOG(___LogChannel___, LOG_LEVEL_DEBUG, LOG_COLOR_DEFAULT, __VA_ARGS__)
#  define TRACE_LOG(...) GENERIC_LOG(___LogChannel___, LOG_LEVEL_TRACE, LOG_COLOR_DEFAULT, __VA_ARGS__)
#else
#  define DEBUG_LOG(...) do { } while (0)
#  define TRACE_LOG(...) do { } while (0)
#endif

#define ERROR_COLOR_LOG(color, ...)   GENERIC_FUNC_LOG(___LogChannel___, LOG_LEVEL_ERROR,   LOG_COLOR_##color, __VA_ARGS__)
#define WARNING_COLOR_LOG(color, ...) GENERIC_FUNC_LOG(___LogChannel___, LOG_LEVEL_WARNING, LOG_COLOR_##color, __VA_ARGS__)
#define INFO_COLOR_LOG(color, ...)    GENERIC_LOG     (___LogChannel___, LOG_LEVEL_INFO,    LOG_COLOR_##color, __VA_ARGS__)
#define VERBOSE_COLOR_LOG(color, ...) GENERIC_LOG     (___LogChannel___, LOG_LEVEL_VERBOSE, LOG_COLOR_##color, __VA_ARGS__)
#define DEV_COLOR_LOG(color, ...)     GENERIC_LOG     (___LogChannel___, LOG_LEVEL_DEV,     LOG_COLOR_##color, __VA_ARGS__)

#if !defined(NDEBUG)
#  define DEBUG_COLOR_LOG(color, ...) GENERIC_LOG(___LogChannel___, LOG_LEVEL_DEBUG, LOG_COLOR_##color, __VA_ARGS__)
#  define TRACE_COLOR_LOG(color, ...) GENERIC_LOG(___LogChannel___, LOG_LEVEL_TRACE, LOG_COLOR_##color, __VA_ARGS__)
#else
#  define DEBUG_COLOR_LOG(color, ...) do { } while (0)
#  define TRACE_COLOR_LOG(color, ...) do { } while (0)
#endif

#endif /* CUPID_COMMON_LOG_H */
