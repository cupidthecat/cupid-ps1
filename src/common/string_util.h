/*
 * StringUtil:: namespace collapsed to string_util_* prefix.  std::string_view
 * args become (const char* data, u32 len) pairs (suffix _view), std::string
 * returns become small_string_t* out-params, std::optional<T> becomes a bool
 * return + out-pointer, std::vector<T> becomes a heap (T*, size_t) pair plus
 * a free helper, std::span<T> becomes a (T* data, size_t count) pair.
 *
 * FromChars/ToChars are emitted per concrete type (s32/u32/s64/u64/float/
 * double + bool).  Pure-template helpers (IsInStringList, AddToStringList,
 * RemoveFromStringList, JoinString) are skipped - call sites will rewrite
 * ad-hoc when ported.
 *
 * Windows-only UTF-16/wide-string helpers from the original header are
 * dropped (Linux only).
 */

#ifndef CUPID_COMMON_STRING_UTIL_H
#define CUPID_COMMON_STRING_UTIL_H

#include "small_string.h"
#include "types.h"

#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <uchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Unicode replacement character. */
#define STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER ((char32_t)0xFFFDu)

ALWAYS_INLINE char string_util_to_lower(char ch)
{
  return (char)(ch + (((u8)ch >= 'A' && (u8)ch <= 'Z') ? ('a' - 'A') : 0));
}

ALWAYS_INLINE char string_util_to_upper(char ch)
{
  return (char)(ch - (((u8)ch >= 'a' && (u8)ch <= 'z') ? ('a' - 'A') : 0));
}

ALWAYS_INLINE bool string_util_is_whitespace(char ch)
{
  return ((ch >= 0x09 && ch <= 0x0D) || ch == 0x20);
}

ALWAYS_INLINE bool string_util_is_hex_digit(char ch)
{
  return ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F') || (ch >= '0' && ch <= '9'));
}

ALWAYS_INLINE u8 string_util_decode_hex_digit(char ch)
{
  if (ch >= '0' && ch <= '9') return (u8)(ch - '0');
  if (ch >= 'a' && ch <= 'f') return (u8)(0xA + (ch - 'a'));
  if (ch >= 'A' && ch <= 'F') return (u8)(0xA + (ch - 'A'));
  return 0;
}

bool   string_util_wildcard_match(const char* subject, const char* mask, bool case_sensitive);
size_t string_util_strlcpy_cstr  (char* dst, const char* src, size_t size);
size_t string_util_strlcpy_view  (char* dst, const char* data, u32 len, size_t size);
size_t string_util_strnlen       (const char* str, size_t max_size);

ALWAYS_INLINE int string_util_strcasecmp(const char* s1, const char* s2)
{
  return strcasecmp(s1, s2);
}
ALWAYS_INLINE int string_util_strncasecmp(const char* s1, const char* s2, size_t n)
{
  return strncasecmp(s1, s2, n);
}

bool string_util_equal_no_case_view  (const char* d1, u32 l1, const char* d2, u32 l2);
int  string_util_compare_no_case_view(const char* d1, u32 l1, const char* d2, u32 l2);
bool string_util_contains_no_case_view(const char* d1, u32 l1, const char* d2, u32 l2);

bool string_util_starts_with_no_case_view(const char* data, u32 len, const char* prefix, u32 prefix_len);
bool string_util_ends_with_no_case_view  (const char* data, u32 len, const char* suffix, u32 suffix_len);

ALWAYS_INLINE int string_util_constexpr_compare(const char* s1, const char* s2)
{
  return strcmp(s1, s2);
}

bool string_util_from_chars_s32(const char* data, u32 len, int base, s32* out, u32* out_consumed);
bool string_util_from_chars_u32(const char* data, u32 len, int base, u32* out, u32* out_consumed);
bool string_util_from_chars_s64(const char* data, u32 len, int base, s64* out, u32* out_consumed);
bool string_util_from_chars_u64(const char* data, u32 len, int base, u64* out, u32* out_consumed);
bool string_util_from_chars_float (const char* data, u32 len, float*  out, u32* out_consumed);
bool string_util_from_chars_double(const char* data, u32 len, double* out, u32* out_consumed);

/* "0x..", "0b..", leading-0 octal sniffer; otherwise base 10. */
bool string_util_from_chars_with_optional_base_s32(const char* data, u32 len, s32* out, u32* out_consumed);
bool string_util_from_chars_with_optional_base_u32(const char* data, u32 len, u32* out, u32* out_consumed);
bool string_util_from_chars_with_optional_base_s64(const char* data, u32 len, s64* out, u32* out_consumed);
bool string_util_from_chars_with_optional_base_u64(const char* data, u32 len, u64* out, u32* out_consumed);

/* "true"/"yes"/"on"/"1"/"enabled" -> true; "false"/"no"/"off"/"0"/"disabled" -> false. */
bool string_util_from_chars_bool(const char* data, u32 len, bool* out);

void string_util_to_chars_s32   (small_string_t* out, s32 value, int base);
void string_util_to_chars_u32   (small_string_t* out, u32 value, int base);
void string_util_to_chars_s64   (small_string_t* out, s64 value, int base);
void string_util_to_chars_u64   (small_string_t* out, u64 value, int base);
void string_util_to_chars_float (small_string_t* out, float  value);
void string_util_to_chars_double(small_string_t* out, double value);
void string_util_to_chars_bool  (small_string_t* out, bool value);

void string_util_strip_control_characters_view(small_string_t* out, const char* data, u32 len);

void string_util_strip_whitespace_view(const char* data, u32 len, const char** out_data, u32* out_len);
/* Strip whitespace operating in place on (data, *len) buffer. *len is updated. */
void string_util_strip_whitespace_inplace(char* data, size_t* len);

void string_util_ellipsise_view   (small_string_t* out, const char* data, u32 len, u32 max_length, const char* ellipsis);
void string_util_ellipsise_inplace(char* data, size_t* len, u32 max_length, const char* ellipsis);

/* Decodes hex pairs into dest_len bytes; returns number of bytes written
 * (== dest_len on success, otherwise the count up to the first failure). */
size_t string_util_decode_hex(u8* dest, size_t dest_len, const char* data, u32 len);
/* Encodes length bytes as 2*length lowercase hex chars and appends to out. */
void   string_util_encode_hex(small_string_t* out, const void* data, size_t length);

/* Parse a fixed-length hex string into out_len bytes. Stops at NUL or end. */
void string_util_parse_fixed_hex_string(const char* str, u8* out, size_t out_len);

ALWAYS_INLINE size_t string_util_decoded_base64_length(const char* data, u32 len)
{
  if ((len % 4u) != 0u) return 0;
  size_t padding = 0;
  if (len >= 2u) {
    padding += (data[len - 1u] == '=') ? 1u : 0u;
    padding += (data[len - 2u] == '=') ? 1u : 0u;
  }
  return ((size_t)len / 4u) * 3u - padding;
}

ALWAYS_INLINE size_t string_util_encoded_base64_length(size_t data_len)
{
  return ((data_len + 2u) / 3u) * 4u;
}

size_t string_util_decode_base64(u8* dest, size_t dest_len, const char* data, u32 len);
size_t string_util_encode_base64(char* dest, size_t dest_len, const u8* data, size_t data_len);
void   string_util_encode_base64_to_small_string(small_string_t* out, const u8* data, size_t data_len);
/* Decodes base64; allocates *out_data on heap (caller frees with free()). */
bool   string_util_decode_base64_alloc(u8** out_data, size_t* out_len, const char* data, u32 len);

size_t string_util_count_char         (const char* data, u32 len, char ch);
size_t string_util_count_char_no_case (const char* data, u32 len, char ch);

typedef struct string_util_split_iter {
  const char* data;
  u32         len;
  u32         pos;
  char        delimiter;
  bool        skip_empty;
} string_util_split_iter_t;

/* Initializes iter. Returns first token (trimmed of whitespace). */
bool string_util_split_first(string_util_split_iter_t* iter,
                             const char* data, u32 len, char delimiter, bool skip_empty,
                             const char** out_token, u32* out_token_len);
/* Advances iter; returns next token. Returns false when exhausted. */
bool string_util_split_next (string_util_split_iter_t* iter,
                             const char** out_token, u32* out_token_len);

void string_util_replace_view(small_string_t* out,
                              const char* subject, u32 subject_len,
                              const char* search,  u32 search_len,
                              const char* replacement, u32 replacement_len);
void string_util_replace_inplace(small_string_t* subject,
                                 const char* search, u32 search_len,
                                 const char* replacement, u32 replacement_len);
void string_util_replace_char_view   (small_string_t* out,
                                      const char* subject, u32 subject_len,
                                      char search, char replacement);
void string_util_replace_char_inplace(small_string_t* subject, char search, char replacement);

bool string_util_parse_assignment_string(const char* data, u32 len,
                                         const char** out_key, u32* out_key_len,
                                         const char** out_value, u32* out_value_len);

/* Pulls token before next separator out of caret. Caret advances past the
 * separator on success.  Returns false (and leaves caret unchanged) when the
 * separator is not present. */
bool string_util_get_next_token(const char** caret_data, u32* caret_len, char separator,
                                const char** out_token, u32* out_token_len);

size_t string_util_get_utf8_character_count(const char* data, u32 len);

/* Append a codepoint to the small_string. */
void   string_util_encode_and_append_utf8 (small_string_t* out, char32_t ch);
/* Encode at (utf8 + pos), bounded by [pos, size). Returns bytes written or 0 on overflow. */
size_t string_util_encode_and_append_utf8_buffer(void* utf8, size_t pos, size_t size, char32_t ch);
size_t string_util_get_encoded_utf8_length(char32_t ch);

/* Decodes one codepoint from bytes[0..length); returns bytes consumed. */
size_t string_util_decode_utf8(const void* bytes, size_t length, char32_t* ch);
size_t string_util_decode_utf8_view(const char* data, u32 len, size_t offset, char32_t* ch);

size_t string_util_encode_and_append_utf16(void* utf16, size_t pos, size_t size, char32_t codepoint);

size_t string_util_decode_utf16   (const void* bytes, size_t pos, size_t size, char32_t* codepoint);
size_t string_util_decode_utf16_be(const void* bytes, size_t pos, size_t size, char32_t* codepoint);

/* size is in bytes; output is appended to out as UTF-8. */
void string_util_decode_utf16_string   (small_string_t* out, const void* bytes, size_t size);
void string_util_decode_utf16_be_string(small_string_t* out, const void* bytes, size_t size);

bool string_util_byte_pattern_search(const u8* bytes, size_t bytes_len,
                                     const char* pattern, u32 pattern_len,
                                     size_t* out_offset);

void string_util_stride_mem_cpy(void* dst, size_t dst_stride,
                                const void* src, size_t src_stride,
                                size_t copy_size, size_t count);
int  string_util_stride_mem_cmp(const void* p1, size_t p1_stride,
                                const void* p2, size_t p2_stride,
                                size_t copy_size, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_COMMON_STRING_UTIL_H */
