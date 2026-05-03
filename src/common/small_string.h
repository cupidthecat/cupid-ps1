/*
 * Small-string buffers with stack-allocated storage.
 *
 *   small_string_t                                  base struct
 *   tiny_string_t  (64 bytes)                       tiny_string_init
 *   small_string_stack_t (256 bytes)                small_string_stack_init
 *   large_string_t (512 bytes)                      large_string_init
 *
 * Formatting uses printf-style (small_string_*_sprintf).  String slices
 * pass as (const char* data, u32 len) pairs.
 *
 * Lifecycle:
 *   tiny_string_t ts; tiny_string_init(&ts);
 *   small_string_assign(&ts.s, "hello");
 *   ...
 *   small_string_destroy(&ts.s);   // safe even on stack-only data
 */
#ifndef CUPID_COMMON_SMALL_STRING_H
#define CUPID_COMMON_SMALL_STRING_H

#include "types.h"

#include <stdarg.h>
#include <stdint.h>

#define SMALL_STRING_NPOS ((s32)-1)

typedef struct small_string {
  char* buffer;       /* always NUL-terminated when buffer != NULL */
  u32   length;       /* length excluding NUL */
  u32   buffer_size;  /* total buffer bytes including NUL slot */
  bool  on_heap;      /* true when buffer was malloc()'d, false for stack */
} small_string_t;

/* Initializer for a heap-only small_string_t (no embedded stack).
 * Equivalent to default SmallStringBase(): nothing reserved. */
void small_string_init(small_string_t* s);

/* Initializer that wraps an externally provided buffer (typically the tail
 * char array of a stack variant).  Buffer is treated as size cap; first byte
 * is set to NUL. */
void small_string_init_stack(small_string_t* s, char* buf, u32 buf_size);

void small_string_destroy(small_string_t* s);

void small_string_assign        (small_string_t* s, const small_string_t* src);
void small_string_assign_cstr   (small_string_t* s, const char* str);
void small_string_assign_view   (small_string_t* s, const char* data, u32 len);
/* "move" assign: if src is heap-allocated, takes ownership of src's heap;
 * otherwise copies.  After call src is empty (length 0). */
void small_string_move_assign   (small_string_t* s, small_string_t* src);

void small_string_make_room_for(small_string_t* s, u32 extra);
void small_string_reserve(small_string_t* s, u32 new_reserve);
void small_string_resize (small_string_t* s, u32 new_size, char fill, bool shrink_if_smaller);
void small_string_set_size(small_string_t* s, u32 new_size, bool shrink_if_smaller);
void small_string_update_size(small_string_t* s);
void small_string_shrink_to_fit(small_string_t* s);
void small_string_clear(small_string_t* s);

ALWAYS_INLINE u32         small_string_length(const small_string_t* s)            { return s->length; }
ALWAYS_INLINE bool        small_string_empty(const small_string_t* s)             { return s->length == 0; }
ALWAYS_INLINE u32         small_string_buffer_size(const small_string_t* s)       { return s->buffer_size; }
ALWAYS_INLINE const char* small_string_c_str(const small_string_t* s)             { return s->buffer ? s->buffer : ""; }
ALWAYS_INLINE char*       small_string_data(small_string_t* s)                    { return s->buffer; }
ALWAYS_INLINE const char* small_string_end_ptr(const small_string_t* s)           { return s->buffer + s->length; }
ALWAYS_INLINE bool        small_string_is_heap_allocated(const small_string_t* s) { return s->on_heap; }

void small_string_append_char (small_string_t* s, char c);
void small_string_append_cstr (small_string_t* s, const char* str);
void small_string_append_view (small_string_t* s, const char* data, u32 len);
void small_string_append      (small_string_t* s, const small_string_t* other);

void small_string_append_sprintf (small_string_t* s, const char* fmt, ...) PRINTFLIKE(2, 3);
void small_string_append_vsprintf(small_string_t* s, const char* fmt, va_list ap);
void small_string_append_hex     (small_string_t* s, const void* data, size_t len, bool comma_separate);

void small_string_prepend_char (small_string_t* s, char c);
void small_string_prepend_cstr (small_string_t* s, const char* str);
void small_string_prepend_view (small_string_t* s, const char* data, u32 len);
void small_string_prepend      (small_string_t* s, const small_string_t* other);

void small_string_prepend_sprintf (small_string_t* s, const char* fmt, ...) PRINTFLIKE(2, 3);
void small_string_prepend_vsprintf(small_string_t* s, const char* fmt, va_list ap);

/* Negative offset counts from end (clamped to 0). */
void small_string_insert_cstr(small_string_t* s, s32 offset, const char* str);
void small_string_insert_view(small_string_t* s, s32 offset, const char* data, u32 len);

void small_string_sprintf (small_string_t* s, const char* fmt, ...) PRINTFLIKE(2, 3);
void small_string_vsprintf(small_string_t* s, const char* fmt, va_list ap);

bool small_string_equals_cstr(const small_string_t* s, const char* str);
bool small_string_equals     (const small_string_t* s, const small_string_t* other);
bool small_string_equals_view(const small_string_t* s, const char* data, u32 len);

bool small_string_iequals_cstr(const small_string_t* s, const char* str);
bool small_string_iequals     (const small_string_t* s, const small_string_t* other);
bool small_string_iequals_view(const small_string_t* s, const char* data, u32 len);

int small_string_compare_cstr(const small_string_t* s, const char* str);
int small_string_compare     (const small_string_t* s, const small_string_t* other);
int small_string_compare_view(const small_string_t* s, const char* data, u32 len);

int small_string_icompare_cstr(const small_string_t* s, const char* str);
int small_string_icompare     (const small_string_t* s, const small_string_t* other);
int small_string_icompare_view(const small_string_t* s, const char* data, u32 len);

bool small_string_starts_with_cstr(const small_string_t* s, const char* str, bool case_sensitive);
bool small_string_starts_with_view(const small_string_t* s, const char* data, u32 len, bool case_sensitive);
bool small_string_ends_with_cstr  (const small_string_t* s, const char* str, bool case_sensitive);
bool small_string_ends_with_view  (const small_string_t* s, const char* data, u32 len, bool case_sensitive);

s32  small_string_find_char (const small_string_t* s, char c, u32 offset);
s32  small_string_rfind_char(const small_string_t* s, char c, u32 offset);
s32  small_string_find_cstr (const small_string_t* s, const char* str, u32 offset);
u32  small_string_count_char(const small_string_t* s, char c);
u32  small_string_replace   (small_string_t* s, const char* search, const char* replacement);

/* count<0 → to end-of-string; offset<0 → from end */
void small_string_erase(small_string_t* s, s32 offset, s32 count);

/* Returns pointer + length into s->buffer (no allocation). */
void small_string_substr(const small_string_t* s, s32 offset, s32 count,
                         const char** out_data, u32* out_len);

void small_string_to_lower(small_string_t* s);
void small_string_to_upper(small_string_t* s);

ALWAYS_INLINE void small_string_view(const small_string_t* s, const char** out_data, u32* out_len)
{
  *out_data = s->buffer;
  *out_len  = s->length;
}

#define SMALL_STRING_DECL_STACK(name_t, init_fn, capacity)                                                             \
  typedef struct {                                                                                                     \
    small_string_t s;                                                                                                  \
    char           stack[capacity];                                                                                    \
  } name_t;                                                                                                            \
  static ALWAYS_INLINE void init_fn(name_t* p) { small_string_init_stack(&p->s, p->stack, (u32)sizeof(p->stack)); }

SMALL_STRING_DECL_STACK(tiny_string_t,        tiny_string_init,        64)
SMALL_STRING_DECL_STACK(small_string_stack_t, small_string_stack_init, 256)
SMALL_STRING_DECL_STACK(large_string_t,       large_string_init,       512)

#undef SMALL_STRING_DECL_STACK

void tiny_string_make_sprintf       (tiny_string_t* out,        const char* fmt, ...) PRINTFLIKE(2, 3);
void small_string_stack_make_sprintf(small_string_stack_t* out, const char* fmt, ...) PRINTFLIKE(2, 3);
void large_string_make_sprintf      (large_string_t* out,       const char* fmt, ...) PRINTFLIKE(2, 3);

#endif /* CUPID_COMMON_SMALL_STRING_H */
