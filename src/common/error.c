#include "error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void error_replace_desc(Error* e, char* new_desc, size_t new_len)
{
  free(e->desc);
  e->desc = new_desc;
  e->len  = new_len;
}

/* Take ownership of a heap buffer that holds new_len bytes (no NUL guarantee
 * yet); caller already wrote those bytes.  We ensure NUL term and store. */
static void error_take(Error* e, char* buf, size_t len)
{
  buf[len] = '\0';
  error_replace_desc(e, buf, len);
}

/* vsnprintf-into-malloc.  Returns NULL on alloc fail; *out_len set on success. */
static char* error_vasprintf(const char* fmt, va_list ap, size_t* out_len)
{
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (n < 0)
    return NULL;
  char* buf = (char*)malloc((size_t)n + 1);
  if (!buf)
    return NULL;
  vsnprintf(buf, (size_t)n + 1, fmt, ap);
  *out_len = (size_t)n;
  return buf;
}

void Error_init(Error* e)
{
  e->type = ERROR_TYPE_NONE;
  e->desc = NULL;
  e->len  = 0;
}

void Error_destroy(Error* e)
{
  if (!e) return;
  free(e->desc);
  e->desc = NULL;
  e->len  = 0;
  e->type = ERROR_TYPE_NONE;
}

void Error_clear(Error* e)
{
  if (!e) return;
  free(e->desc);
  e->desc = NULL;
  e->len  = 0;
  e->type = ERROR_TYPE_NONE;
}

bool Error_is_valid(const Error* e)               { return e != NULL && e->type != ERROR_TYPE_NONE; }
ErrorType Error_get_type(const Error* e)          { return e ? e->type : ERROR_TYPE_NONE; }
const char* Error_get_description(const Error* e) { return (e && e->desc) ? e->desc : ""; }

void Error_set_errno(Error* e, int err)
{
  Error_set_errno_prefix(e, NULL, err);
}

void Error_set_errno_prefix(Error* e, const char* prefix, int err)
{
  if (!e) return;
  e->type = ERROR_TYPE_ERRNO;
  const char* msg = strerror(err);
  Error_set_string_fmt(e, "%serrno %d: %s", prefix ? prefix : "", err,
                       msg ? msg : "<Could not get error message>");
  /* set_string_fmt overrode type to USER; restore. */
  e->type = ERROR_TYPE_ERRNO;
}

void Error_set_socket(Error* e, int err)
{
  Error_set_socket_prefix(e, NULL, err);
}

void Error_set_socket_prefix(Error* e, const char* prefix, int err)
{
  if (!e) return;
  Error_set_errno_prefix(e, prefix, err);
  e->type = ERROR_TYPE_SOCKET;
}

void Error_set_string(Error* e, const char* description)
{
  if (!e) return;
  Error_set_string_n(e, description, description ? strlen(description) : 0);
}

void Error_set_string_n(Error* e, const char* description, size_t len)
{
  if (!e) return;
  e->type = ERROR_TYPE_USER;
  if (!description || len == 0) {
    error_replace_desc(e, NULL, 0);
    return;
  }
  char* buf = (char*)malloc(len + 1);
  if (!buf) {
    error_replace_desc(e, NULL, 0);
    return;
  }
  memcpy(buf, description, len);
  error_take(e, buf, len);
}

void Error_set_string_fmt(Error* e, const char* fmt, ...)
{
  if (!e) return;
  va_list ap;
  va_start(ap, fmt);
  Error_set_string_vfmt(e, fmt, ap);
  va_end(ap);
}

void Error_set_string_vfmt(Error* e, const char* fmt, va_list ap)
{
  if (!e) return;
  e->type = ERROR_TYPE_USER;
  size_t n = 0;
  char* buf = error_vasprintf(fmt, ap, &n);
  if (!buf) {
    error_replace_desc(e, NULL, 0);
    return;
  }
  error_replace_desc(e, buf, n);
}

static void error_concat(Error* e, const char* a, size_t alen, const char* b, size_t blen)
{
  char* buf = (char*)malloc(alen + blen + 1);
  if (!buf) return; /* keep existing */
  if (alen) memcpy(buf, a, alen);
  if (blen) memcpy(buf + alen, b, blen);
  error_take(e, buf, alen + blen);
}

void Error_add_prefix(Error* e, const char* prefix)
{
  if (!e || !prefix) return;
  size_t plen = strlen(prefix);
  if (plen == 0) return;
  error_concat(e, prefix, plen, e->desc ? e->desc : "", e->len);
}

void Error_add_suffix(Error* e, const char* suffix)
{
  if (!e || !suffix) return;
  size_t slen = strlen(suffix);
  if (slen == 0) return;
  error_concat(e, e->desc ? e->desc : "", e->len, suffix, slen);
}

void Error_add_prefix_fmt(Error* e, const char* fmt, ...)
{
  if (!e) return;
  va_list ap;
  va_start(ap, fmt);
  size_t n = 0;
  char* buf = error_vasprintf(fmt, ap, &n);
  va_end(ap);
  if (!buf) return;
  error_concat(e, buf, n, e->desc ? e->desc : "", e->len);
  free(buf);
}

void Error_add_suffix_fmt(Error* e, const char* fmt, ...)
{
  if (!e) return;
  va_list ap;
  va_start(ap, fmt);
  size_t n = 0;
  char* buf = error_vasprintf(fmt, ap, &n);
  va_end(ap);
  if (!buf) return;
  error_concat(e, e->desc ? e->desc : "", e->len, buf, n);
  free(buf);
}
