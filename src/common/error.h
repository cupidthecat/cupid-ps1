/*
 * C++ Error class collapsed to struct Error + Error_* funcs.  fmt:: replaced
 * with printf-style varargs.  Win32/HResult/Socket-on-Windows paths dropped.
 *
 * All Error_* setters tolerate a NULL Error* - callers may freely pass NULL
 *
 * Lifecycle: Error e = ERROR_INIT;  use ...  Error_destroy(&e);
 */

#ifndef CUPID_COMMON_ERROR_H
#define CUPID_COMMON_ERROR_H

#include "types.h"

#include <stdarg.h>

typedef enum {
  ERROR_TYPE_NONE   = 0,
  ERROR_TYPE_ERRNO  = 1,
  ERROR_TYPE_SOCKET = 2,
  ERROR_TYPE_USER   = 3,
} ErrorType;

typedef struct Error {
  ErrorType type;
  char*     desc;   /* heap, NUL-terminated; NULL when type == NONE */
  size_t    len;    /* byte length excluding NUL */
} Error;

#define ERROR_INIT ((Error){ ERROR_TYPE_NONE, NULL, 0 })

void Error_init(Error* e);
void Error_destroy(Error* e);
void Error_clear(Error* e);

bool        Error_is_valid(const Error* e);
ErrorType   Error_get_type(const Error* e);
const char* Error_get_description(const Error* e); /* "" when none */

/* All setters/adders accept NULL e and silently discard. */

void Error_set_errno(Error* e, int err);
void Error_set_errno_prefix(Error* e, const char* prefix, int err);

void Error_set_socket(Error* e, int err);
void Error_set_socket_prefix(Error* e, const char* prefix, int err);

void Error_set_string(Error* e, const char* description);
void Error_set_string_n(Error* e, const char* description, size_t len);

void Error_set_string_fmt(Error* e, const char* fmt, ...) PRINTFLIKE(2, 3);
void Error_set_string_vfmt(Error* e, const char* fmt, va_list ap);

void Error_add_prefix(Error* e, const char* prefix);
void Error_add_suffix(Error* e, const char* suffix);
void Error_add_prefix_fmt(Error* e, const char* fmt, ...) PRINTFLIKE(2, 3);
void Error_add_suffix_fmt(Error* e, const char* fmt, ...) PRINTFLIKE(2, 3);

#endif /* CUPID_COMMON_ERROR_H */
