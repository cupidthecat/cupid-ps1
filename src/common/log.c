/*
 *

 * fmt::, std::vector / std::mutex.  Console output uses ANSI escape codes on
 * stderr/stdout based on level.
 */
#include "log.h"

#include "assert.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* const s_channel_names[LOG_CHANNEL_MAX_COUNT] = {
#define LOG_CHANNEL_NAME(X) #X,
  ENUMERATE_LOG_CHANNELS(LOG_CHANNEL_NAME)
#undef LOG_CHANNEL_NAME
};

static const char s_level_chars[LOG_LEVEL_MAX_COUNT] = {
  'X', 'E', 'W', 'I', 'V', 'D', 'B', 'T'
};

static const char* const s_ansi_codes[LOG_COLOR_MAX_COUNT] = {
  "\033[0m",          /* default */
  "\033[30m\033[1m",  /* black */
  "\033[31m",         /* red */
  "\033[32m",         /* green */
  "\033[34m",         /* blue */
  "\033[35m",         /* magenta */
  "\033[38;5;217m",   /* orange */
  "\033[36m",         /* cyan */
  "\033[33m",         /* yellow */
  "\033[37m",         /* white */
  "\033[30m\033[1m",  /* strong black */
  "\033[31m\033[1m",  /* strong red */
  "\033[32m\033[1m",  /* strong green */
  "\033[34m\033[1m",  /* strong blue */
  "\033[35m\033[1m",  /* strong magenta */
  "\033[38;5;202m",   /* strong orange */
  "\033[36m\033[1m",  /* strong cyan */
  "\033[33m\033[1m",  /* strong yellow */
  "\033[37m\033[1m",  /* strong white */
};

#define LOG_MAX_CALLBACKS 8
#define LOG_MSG_STACK_BUF 1024

typedef struct {
  log_callback_t fn;
  void*          user;
} log_registered_cb_t;

static struct {
  log_level_t      effective_level;
  log_level_t      requested_level;
  u8               channel_enabled[(LOG_CHANNEL_MAX_COUNT + 7) / 8];

  log_registered_cb_t callbacks[LOG_MAX_CALLBACKS];
  unsigned         callback_count;
  pthread_mutex_t  mu;

  struct timespec  start_ts;

  FILE*            file_handle;
  bool             console_enabled;
  bool             console_timestamps;
  bool             file_enabled;
  bool             file_timestamps;
} s_log = {
  .effective_level = LOG_LEVEL_NONE,
  .requested_level = LOG_LEVEL_TRACE,
  .mu = PTHREAD_MUTEX_INITIALIZER,
};

static void log_console_cb(void* user, u32 cat, const char* func, const char* msg, size_t msg_len);
static void log_file_cb   (void* user, u32 cat, const char* func, const char* msg, size_t msg_len);

static bool channel_is_enabled(log_channel_t ch)
{
  if ((unsigned)ch >= LOG_CHANNEL_MAX_COUNT) return false;
  return (s_log.channel_enabled[ch >> 3] & (1u << (ch & 7))) != 0;
}
static void channel_set_enabled(log_channel_t ch, bool en)
{
  if ((unsigned)ch >= LOG_CHANNEL_MAX_COUNT) return;
  if (en) s_log.channel_enabled[ch >> 3] |=  (u8)(1u << (ch & 7));
  else    s_log.channel_enabled[ch >> 3] &= (u8)~(1u << (ch & 7));
}

static void log_init_once(void)
{
  static bool inited = false;
  if (inited) return;
  inited = true;
  for (size_t i = 0; i < sizeof(s_log.channel_enabled); i++)
    s_log.channel_enabled[i] = 0xFF;
  clock_gettime(CLOCK_MONOTONIC, &s_log.start_ts);
}

static void update_effective_level_locked(void)
{
  s_log.effective_level = (s_log.callback_count == 0) ? LOG_LEVEL_NONE : s_log.requested_level;
}

static void register_cb_locked(log_callback_t fn, void* user)
{
  if (s_log.callback_count >= LOG_MAX_CALLBACKS)
    return;
  s_log.callbacks[s_log.callback_count].fn   = fn;
  s_log.callbacks[s_log.callback_count].user = user;
  s_log.callback_count++;
  update_effective_level_locked();
}

static void unregister_cb_locked(log_callback_t fn, void* user)
{
  for (unsigned i = 0; i < s_log.callback_count; i++) {
    if (s_log.callbacks[i].fn == fn && s_log.callbacks[i].user == user) {
      for (unsigned j = i + 1; j < s_log.callback_count; j++)
        s_log.callbacks[j - 1] = s_log.callbacks[j];
      s_log.callback_count--;
      update_effective_level_locked();
      return;
    }
  }
}

void log_register_callback(log_callback_t fn, void* user)
{
  log_init_once();
  pthread_mutex_lock(&s_log.mu);
  register_cb_locked(fn, user);
  pthread_mutex_unlock(&s_log.mu);
}

void log_unregister_callback(log_callback_t fn, void* user)
{
  pthread_mutex_lock(&s_log.mu);
  unregister_cb_locked(fn, user);
  pthread_mutex_unlock(&s_log.mu);
}

const char* log_get_channel_name(log_channel_t ch)
{
  return ((unsigned)ch < LOG_CHANNEL_MAX_COUNT) ? s_channel_names[ch] : "?";
}

log_color_t log_get_color_for_level(log_level_t lv)
{
  static const log_color_t per_level[LOG_LEVEL_MAX_COUNT] = {
    LOG_COLOR_DEFAULT,        /* None */
    LOG_COLOR_STRONG_RED,     /* Error */
    LOG_COLOR_STRONG_YELLOW,  /* Warning */
    LOG_COLOR_STRONG_WHITE,   /* Info */
    LOG_COLOR_STRONG_GREEN,   /* Verbose */
    LOG_COLOR_WHITE,          /* Dev */
    LOG_COLOR_GREEN,          /* Debug */
    LOG_COLOR_BLUE,           /* Trace */
  };
  return ((unsigned)lv < LOG_LEVEL_MAX_COUNT) ? per_level[lv] : LOG_COLOR_DEFAULT;
}

float log_get_current_message_time(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  double sec = (double)(now.tv_sec  - s_log.start_ts.tv_sec) +
               (double)(now.tv_nsec - s_log.start_ts.tv_nsec) * 1e-9;
  return (float)sec;
}

bool log_are_console_output_timestamps_enabled(void) { return s_log.console_timestamps; }
bool log_is_console_output_enabled(void)             { return s_log.console_enabled; }

log_level_t log_get_log_level(void)                        { return s_log.effective_level; }
bool log_is_log_visible(log_level_t lv, log_channel_t ch)
{
  log_init_once();
  return lv <= s_log.effective_level && channel_is_enabled(ch);
}

void log_set_log_level(log_level_t lv)
{
  log_init_once();
  pthread_mutex_lock(&s_log.mu);
  DebugAssert(lv < LOG_LEVEL_MAX_COUNT);
  s_log.requested_level = lv;
  update_effective_level_locked();
  pthread_mutex_unlock(&s_log.mu);
}

void log_set_channel_enabled(log_channel_t ch, bool en)
{
  log_init_once();
  pthread_mutex_lock(&s_log.mu);
  channel_set_enabled(ch, en);
  pthread_mutex_unlock(&s_log.mu);
}

void log_set_console_output_params(bool enabled, bool timestamps)
{
  log_init_once();
  pthread_mutex_lock(&s_log.mu);
  s_log.console_timestamps = timestamps;
  if (s_log.console_enabled != enabled) {
    s_log.console_enabled = enabled;
    if (enabled) register_cb_locked  (log_console_cb, NULL);
    else         unregister_cb_locked(log_console_cb, NULL);
  }
  pthread_mutex_unlock(&s_log.mu);
}

void log_set_file_output_params(bool enabled, const char* filename, bool timestamps)
{
  log_init_once();
  pthread_mutex_lock(&s_log.mu);
  s_log.file_timestamps = timestamps;
  if (s_log.file_enabled == enabled) {
    pthread_mutex_unlock(&s_log.mu);
    return;
  }
  if (enabled) {
    FILE* f = fopen(filename, "wb");
    if (!f) {
      pthread_mutex_unlock(&s_log.mu);
      fprintf(stderr, "log: failed to open %s\n", filename);
      return;
    }
    s_log.file_handle  = f;
    s_log.file_enabled = true;
    register_cb_locked(log_file_cb, NULL);
  } else {
    unregister_cb_locked(log_file_cb, NULL);
    if (s_log.file_handle) {
      fclose(s_log.file_handle);
      s_log.file_handle = NULL;
    }
    s_log.file_enabled = false;
  }
  pthread_mutex_unlock(&s_log.mu);
}

/* Build "[time] L(func): msg\n" or "L/channel: msg\n" into out (size cap).
 * Returns chars written (excluding NUL), capped to cap-1. */
static int format_one_line(char* out, int cap, u32 cat, const char* func, const char* msg, int msg_len,
                           bool timestamp, bool ansi)
{
  const log_level_t lv = log_unpack_level(cat);
  log_color_t col      = log_unpack_color(cat);
  if (col == LOG_COLOR_DEFAULT)
    col = log_get_color_for_level(lv);
  const char* chan_name = log_get_channel_name(log_unpack_channel(cat));
  const char  lvc       = s_level_chars[lv];
  const char* col_s     = ansi ? s_ansi_codes[col] : "";
  const char* col_e     = ansi ? s_ansi_codes[LOG_COLOR_DEFAULT] : "";

  if (timestamp) {
    const float t = log_get_current_message_time();
    if (func)
      return snprintf(out, (size_t)cap, "[%10.4f] %s%c(%s): %.*s%s\n",
                      (double)t, col_s, lvc, func, msg_len, msg, col_e);
    return snprintf(out, (size_t)cap, "[%10.4f] %s%c/%s: %.*s%s\n",
                    (double)t, col_s, lvc, chan_name, msg_len, msg, col_e);
  }
  if (func)
    return snprintf(out, (size_t)cap, "%s%c(%s): %.*s%s\n", col_s, lvc, func, msg_len, msg, col_e);
  return snprintf(out, (size_t)cap, "%s%c/%s: %.*s%s\n", col_s, lvc, chan_name, msg_len, msg, col_e);
}

static void log_console_cb(void* user, u32 cat, const char* func, const char* msg, size_t msg_len)
{
  (void)user;
  if (!s_log.console_enabled) return;
  char stack_buf[LOG_MSG_STACK_BUF];
  char* buf = stack_buf;
  int needed = format_one_line(stack_buf, sizeof(stack_buf), cat, func, msg, (int)msg_len,
                               s_log.console_timestamps, true);
  if (needed >= (int)sizeof(stack_buf)) {
    buf = (char*)malloc((size_t)needed + 1);
    if (!buf) return;
    format_one_line(buf, needed + 1, cat, func, msg, (int)msg_len, s_log.console_timestamps, true);
  }
  const int fd = (log_unpack_level(cat) <= LOG_LEVEL_WARNING) ? STDERR_FILENO : STDOUT_FILENO;
  size_t to_write = (size_t)((needed < (int)sizeof(stack_buf)) ? needed : needed);
  ssize_t w = write(fd, buf, to_write); (void)w;
  if (buf != stack_buf) free(buf);
}

static void log_file_cb(void* user, u32 cat, const char* func, const char* msg, size_t msg_len)
{
  (void)user;
  if (!s_log.file_enabled || !s_log.file_handle) return;
  char stack_buf[LOG_MSG_STACK_BUF];
  char* buf = stack_buf;
  int needed = format_one_line(stack_buf, sizeof(stack_buf), cat, func, msg, (int)msg_len,
                               s_log.file_timestamps, false);
  if (needed >= (int)sizeof(stack_buf)) {
    buf = (char*)malloc((size_t)needed + 1);
    if (!buf) return;
    format_one_line(buf, needed + 1, cat, func, msg, (int)msg_len, s_log.file_timestamps, false);
  }
  fwrite(buf, 1, (size_t)needed, s_log.file_handle);
  fflush(s_log.file_handle);
  if (buf != stack_buf) free(buf);
}

static bool filter_test_locked(log_channel_t ch, log_level_t lv)
{
  return lv <= s_log.effective_level && channel_is_enabled(ch);
}

static void execute_callbacks_locked(u32 cat, const char* func, const char* msg, size_t msg_len)
{
  for (unsigned i = 0; i < s_log.callback_count; i++)
    s_log.callbacks[i].fn(s_log.callbacks[i].user, cat, func, msg, msg_len);
}

void log_write(u32 cat, const char* msg, size_t msg_len)
{
  log_init_once();
  if (!filter_test_locked(log_unpack_channel(cat), log_unpack_level(cat))) return;
  pthread_mutex_lock(&s_log.mu);
  execute_callbacks_locked(cat, NULL, msg, msg_len);
  pthread_mutex_unlock(&s_log.mu);
}

void log_write_func(u32 cat, const char* func, const char* msg, size_t msg_len)
{
  log_init_once();
  if (!filter_test_locked(log_unpack_channel(cat), log_unpack_level(cat))) return;
  pthread_mutex_lock(&s_log.mu);
  execute_callbacks_locked(cat, func, msg, msg_len);
  pthread_mutex_unlock(&s_log.mu);
}

void log_write_vfmt(u32 cat, const char* fmt, va_list ap)
{
  log_init_once();
  if (!filter_test_locked(log_unpack_channel(cat), log_unpack_level(cat))) return;

  char stack_buf[LOG_MSG_STACK_BUF];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap2);
  va_end(ap2);
  if (n < 0) return;

  char* buf = stack_buf;
  if (n >= (int)sizeof(stack_buf)) {
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) return;
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
  }

  pthread_mutex_lock(&s_log.mu);
  execute_callbacks_locked(cat, NULL, buf, (size_t)n);
  pthread_mutex_unlock(&s_log.mu);

  if (buf != stack_buf) free(buf);
}

void log_write_func_vfmt(u32 cat, const char* func, const char* fmt, va_list ap)
{
  log_init_once();
  if (!filter_test_locked(log_unpack_channel(cat), log_unpack_level(cat))) return;

  char stack_buf[LOG_MSG_STACK_BUF];
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap2);
  va_end(ap2);
  if (n < 0) return;

  char* buf = stack_buf;
  if (n >= (int)sizeof(stack_buf)) {
    buf = (char*)malloc((size_t)n + 1);
    if (!buf) return;
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
  }

  pthread_mutex_lock(&s_log.mu);
  execute_callbacks_locked(cat, func, buf, (size_t)n);
  pthread_mutex_unlock(&s_log.mu);

  if (buf != stack_buf) free(buf);
}

void log_write_fmt(u32 cat, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  log_write_vfmt(cat, fmt, ap);
  va_end(ap);
}

void log_write_func_fmt(u32 cat, const char* func, const char* fmt, ...)
{
  va_list ap; va_start(ap, fmt);
  log_write_func_vfmt(cat, func, fmt, ap);
  va_end(ap);
}
