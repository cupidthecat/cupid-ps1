#include "string_util.h"

#include "assert.h"
#include "bitutils.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static size_t size_min(size_t a, size_t b) { return a < b ? a : b; }
static u32    u32_min (u32 a, u32 b)       { return a < b ? a : b; }

/* Lowercase ASCII helper for ContainsNoCase / CountCharNoCase. */
static char ascii_lower(char ch) { return string_util_to_lower(ch); }

/* Copies (data, len) into a NUL-terminated stack/heap buffer.  When len fits
 * in stack_buf, no allocation; otherwise *out_heap is malloc'd and must be
 * free()'d by the caller.  Returns pointer to NUL-terminated copy. */
static char* dup_nul_terminate(const char* data, u32 len, char* stack_buf, size_t stack_cap, char** out_heap)
{
  *out_heap = NULL;
  if ((size_t)len + 1u <= stack_cap) {
    if (len > 0) memcpy(stack_buf, data, len);
    stack_buf[len] = '\0';
    return stack_buf;
  }
  char* p = (char*)malloc((size_t)len + 1u);
  if (!p) Panic("Memory allocation failed.");
  if (len > 0) memcpy(p, data, len);
  p[len] = '\0';
  *out_heap = p;
  return p;
}

bool string_util_wildcard_match(const char* subject, const char* mask, bool case_sensitive)
{
  if (!case_sensitive) {
    const char* cp = NULL;
    const char* mp = NULL;

    while ((*subject) && (*mask != '*')) {
      if ((*mask != '?') && (tolower((unsigned char)*mask) != tolower((unsigned char)*subject)))
        return false;
      mask++;
      subject++;
    }

    while (*subject) {
      if (*mask == '*') {
        if (*++mask == 0)
          return true;
        mp = mask;
        cp = subject + 1;
      } else {
        if ((*mask == '?') || (tolower((unsigned char)*mask) == tolower((unsigned char)*subject))) {
          mask++;
          subject++;
        } else {
          mask = mp;
          subject = cp++;
        }
      }
    }

    while (*mask == '*') mask++;
    return *mask == 0;
  } else {
    const char* cp = NULL;
    const char* mp = NULL;

    while ((*subject) && (*mask != '*')) {
      if ((*mask != *subject) && (*mask != '?'))
        return false;
      mask++;
      subject++;
    }

    while (*subject) {
      if (*mask == '*') {
        if (*++mask == 0)
          return true;
        mp = mask;
        cp = subject + 1;
      } else {
        if ((*mask == *subject) || (*mask == '?')) {
          mask++;
          subject++;
        } else {
          mask = mp;
          subject = cp++;
        }
      }
    }

    while (*mask == '*') mask++;
    return *mask == 0;
  }
}

size_t string_util_strlcpy_cstr(char* dst, const char* src, size_t size)
{
  size_t len = strlen(src);
  if (len < size) {
    memcpy(dst, src, len + 1);
  } else if (size > 0) {
    memcpy(dst, src, size - 1);
    dst[size - 1] = '\0';
  }
  return len;
}

size_t string_util_strlcpy_view(char* dst, const char* data, u32 len, size_t size)
{
  if (len < size) {
    if (len > 0) memcpy(dst, data, len);
    dst[len] = '\0';
  } else if (size > 0) {
    memcpy(dst, data, size - 1);
    dst[size - 1] = '\0';
  }
  return len;
}

size_t string_util_strnlen(const char* str, size_t max_size)
{
  const char* loc = (const char*)memchr(str, 0, max_size);
  return loc ? (size_t)(loc - str) : max_size;
}

bool string_util_equal_no_case_view(const char* d1, u32 l1, const char* d2, u32 l2)
{
  if (l1 != l2) return false;
  if (l1 == 0)  return true;
  return strncasecmp(d1, d2, l1) == 0;
}

int string_util_compare_no_case_view(const char* d1, u32 l1, const char* d2, u32 l2)
{
  const u32 cmp_len = u32_min(l1, l2);
  const int r = (cmp_len > 0) ? strncasecmp(d1, d2, cmp_len) : 0;
  if (r != 0) return r;
  return (l1 < l2) ? -1 : ((l1 > l2) ? 1 : 0);
}

bool string_util_contains_no_case_view(const char* d1, u32 l1, const char* d2, u32 l2)
{
  if (l2 == 0) return true;
  if (l2 > l1) return false;
  const u32 stop = l1 - l2 + 1u;
  for (u32 i = 0; i < stop; i++) {
    if (strncasecmp(d1 + i, d2, l2) == 0)
      return true;
  }
  return false;
}

bool string_util_starts_with_no_case_view(const char* data, u32 len, const char* prefix, u32 prefix_len)
{
  if (prefix_len > len) return false;
  if (prefix_len == 0)  return true;
  return strncasecmp(data, prefix, prefix_len) == 0;
}

bool string_util_ends_with_no_case_view(const char* data, u32 len, const char* suffix, u32 suffix_len)
{
  if (suffix_len > len) return false;
  if (suffix_len == 0)  return true;
  return strncasecmp(data + (len - suffix_len), suffix, suffix_len) == 0;
}

/* Manual integer parser modelled on std::from_chars: no leading whitespace
 * accepted, no leading '+' for unsigned types, base 2..36, sign optional for
 * signed types. Returns number of characters consumed (0 on failure). */
static u32 parse_uint(const char* data, u32 len, int base, u64 max_val, u64* out)
{
  if (len == 0 || base < 2 || base > 36) return 0;
  u64 acc = 0;
  u32 i   = 0;
  bool any = false;
  for (; i < len; i++) {
    const char c = data[i];
    int digit;
    if (c >= '0' && c <= '9')      digit = c - '0';
    else if (c >= 'a' && c <= 'z') digit = 10 + (c - 'a');
    else if (c >= 'A' && c <= 'Z') digit = 10 + (c - 'A');
    else                           break;
    if (digit >= base) break;

    /* Overflow check: acc*base + digit > max_val? */
    if (acc > (max_val - (u64)digit) / (u64)base)
      return 0;
    acc = acc * (u64)base + (u64)digit;
    any = true;
  }
  if (!any) return 0;
  *out = acc;
  return i;
}

static u32 parse_sint(const char* data, u32 len, int base, s64 min_val, s64 max_val, s64* out)
{
  if (len == 0) return 0;
  bool negative = false;
  u32  start    = 0;
  if (data[0] == '-') { negative = true;  start = 1; }
  else if (data[0] == '+') { start = 1; }
  if (start >= len) return 0;

  /* Pick magnitude cap so we don't overflow when re-applying sign. */
  const u64 cap = negative ? (u64)(-(min_val + 1)) + 1u : (u64)max_val;
  u64 mag = 0;
  const u32 consumed = parse_uint(data + start, len - start, base, cap, &mag);
  if (consumed == 0) return 0;

  if (negative) {
    if (mag == (u64)(-(min_val + 1)) + 1u) *out = min_val;
    else                                   *out = -(s64)mag;
  } else {
    *out = (s64)mag;
  }
  return start + consumed;
}

bool string_util_from_chars_s32(const char* data, u32 len, int base, s32* out, u32* out_consumed)
{
  s64 v;
  const u32 c = parse_sint(data, len, base, INT32_MIN, INT32_MAX, &v);
  if (c == 0) return false;
  *out = (s32)v;
  if (out_consumed) *out_consumed = c;
  return true;
}

bool string_util_from_chars_u32(const char* data, u32 len, int base, u32* out, u32* out_consumed)
{
  u64 v;
  const u32 c = parse_uint(data, len, base, UINT32_MAX, &v);
  if (c == 0) return false;
  *out = (u32)v;
  if (out_consumed) *out_consumed = c;
  return true;
}

bool string_util_from_chars_s64(const char* data, u32 len, int base, s64* out, u32* out_consumed)
{
  s64 v;
  const u32 c = parse_sint(data, len, base, INT64_MIN, INT64_MAX, &v);
  if (c == 0) return false;
  *out = v;
  if (out_consumed) *out_consumed = c;
  return true;
}

bool string_util_from_chars_u64(const char* data, u32 len, int base, u64* out, u32* out_consumed)
{
  u64 v;
  const u32 c = parse_uint(data, len, base, UINT64_MAX, &v);
  if (c == 0) return false;
  *out = v;
  if (out_consumed) *out_consumed = c;
  return true;
}

/* Float parsers: copy view into NUL-terminated buffer, use strtof/strtod, then
 * derive consumed count from endptr offset. */
bool string_util_from_chars_float(const char* data, u32 len, float* out, u32* out_consumed)
{
  if (len == 0) return false;
  char  stack_buf[128];
  char* heap = NULL;
  char* buf  = dup_nul_terminate(data, len, stack_buf, sizeof(stack_buf), &heap);

  errno = 0;
  char* end = NULL;
  const float v = strtof(buf, &end);
  if (end == buf || errno == ERANGE) {
    free(heap);
    return false;
  }

  *out = v;
  if (out_consumed) *out_consumed = (u32)(end - buf);
  free(heap);
  return true;
}

bool string_util_from_chars_double(const char* data, u32 len, double* out, u32* out_consumed)
{
  if (len == 0) return false;
  char  stack_buf[128];
  char* heap = NULL;
  char* buf  = dup_nul_terminate(data, len, stack_buf, sizeof(stack_buf), &heap);

  errno = 0;
  char* end = NULL;
  const double v = strtod(buf, &end);
  if (end == buf || errno == ERANGE) {
    free(heap);
    return false;
  }

  *out = v;
  if (out_consumed) *out_consumed = (u32)(end - buf);
  free(heap);
  return true;
}

/* "0x" hex / "0b" binary / leading-0 octal sniffer. */
#define DEFINE_OPTIONAL_BASE(suffix, T)                                                                                \
  bool string_util_from_chars_with_optional_base_##suffix(const char* data, u32 len, T* out, u32* out_consumed)        \
  {                                                                                                                    \
    int  base;                                                                                                         \
    u32  off;                                                                                                          \
    if (len >= 2 && data[0] == '0' && (data[1] == 'x' || data[1] == 'X')) {                                            \
      base = 16; off = 2;                                                                                              \
    } else if (len >= 2 && data[0] == '0' && (data[1] == 'b' || data[1] == 'B')) {                                     \
      base = 2;  off = 2;                                                                                              \
    } else if (len > 1 && data[0] == '0') {                                                                            \
      base = 8;  off = 1;                                                                                              \
    } else {                                                                                                           \
      base = 10; off = 0;                                                                                              \
    }                                                                                                                  \
    u32 consumed = 0;                                                                                                  \
    const bool ok = string_util_from_chars_##suffix(data + off, len - off, base, out, &consumed);                      \
    if (ok && out_consumed) *out_consumed = consumed + off;                                                            \
    return ok;                                                                                                         \
  }

DEFINE_OPTIONAL_BASE(s32, s32)
DEFINE_OPTIONAL_BASE(u32, u32)
DEFINE_OPTIONAL_BASE(s64, s64)
DEFINE_OPTIONAL_BASE(u64, u64)

#undef DEFINE_OPTIONAL_BASE

bool string_util_from_chars_bool(const char* data, u32 len, bool* out)
{
  if (len == 4 && strncasecmp(data, "true",     4) == 0) { *out = true;  return true; }
  if (len == 3 && strncasecmp(data, "yes",      3) == 0) { *out = true;  return true; }
  if (len == 2 && strncasecmp(data, "on",       2) == 0) { *out = true;  return true; }
  if (len == 1 && data[0] == '1')                        { *out = true;  return true; }
  if (len == 7 && strncasecmp(data, "enabled",  7) == 0) { *out = true;  return true; }

  if (len == 5 && strncasecmp(data, "false",    5) == 0) { *out = false; return true; }
  if (len == 2 && strncasecmp(data, "no",       2) == 0) { *out = false; return true; }
  if (len == 3 && strncasecmp(data, "off",      3) == 0) { *out = false; return true; }
  if (len == 1 && data[0] == '0')                        { *out = false; return true; }
  if (len == 8 && strncasecmp(data, "disabled", 8) == 0) { *out = false; return true; }
  return false;
}

/* Integer formatting: hand-roll for arbitrary base (2..36).  printf %d / %x
 * / %o cover most call sites but std::to_chars supports any base, so we
 * mirror that. */
static void format_uint(small_string_t* out, u64 value, int base)
{
  Assert(base >= 2 && base <= 36);
  /* 64 bits in base 2 = 64 chars. */
  char buf[65];
  u32  pos = sizeof(buf);
  buf[--pos] = '\0';
  if (value == 0) {
    buf[--pos] = '0';
  } else {
    while (value != 0) {
      const u32 digit = (u32)(value % (u64)base);
      buf[--pos] = (char)((digit < 10) ? ('0' + digit) : ('a' + digit - 10));
      value /= (u64)base;
    }
  }
  small_string_append_view(out, buf + pos, (u32)(sizeof(buf) - 1 - pos));
}

static void format_sint(small_string_t* out, s64 value, int base)
{
  if (value < 0) {
    small_string_append_char(out, '-');
    /* Guard INT64_MIN: can't negate directly. */
    const u64 mag = (value == INT64_MIN) ? ((u64)INT64_MAX + 1u) : (u64)(-value);
    format_uint(out, mag, base);
  } else {
    format_uint(out, (u64)value, base);
  }
}

void string_util_to_chars_s32(small_string_t* out, s32 value, int base) { format_sint(out, (s64)value, base); }
void string_util_to_chars_u32(small_string_t* out, u32 value, int base) { format_uint(out, (u64)value, base); }
void string_util_to_chars_s64(small_string_t* out, s64 value, int base) { format_sint(out,       value, base); }
void string_util_to_chars_u64(small_string_t* out, u64 value, int base) { format_uint(out,       value, base); }

void string_util_to_chars_float(small_string_t* out, float value)
{
  /* %.9g matches std::to_chars round-trip width for float. */
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%.9g", (double)value);
  if (n > 0)
    small_string_append_view(out, buf, (u32)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
}

void string_util_to_chars_double(small_string_t* out, double value)
{
  /* %.17g is the round-trip width for double. */
  char buf[64];
  const int n = snprintf(buf, sizeof(buf), "%.17g", value);
  if (n > 0)
    small_string_append_view(out, buf, (u32)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
}

void string_util_to_chars_bool(small_string_t* out, bool value)
{
  small_string_append_cstr(out, value ? "true" : "false");
}

void string_util_strip_control_characters_view(small_string_t* out, const char* data, u32 len)
{
  for (size_t i = 0; i < len;) {
    char32_t ch;
    i += string_util_decode_utf8(data + i, len - i, &ch);
    if (ch < 0x20) ch = '_';
    string_util_encode_and_append_utf8(out, ch);
  }
}

void string_util_strip_whitespace_view(const char* data, u32 len, const char** out_data, u32* out_len)
{
  u32 start = 0;
  while (start < len && string_util_is_whitespace(data[start])) start++;
  if (start == len) {
    *out_data = NULL;
    *out_len  = 0;
    return;
  }
  u32 end = len - 1u;
  while (end > start && string_util_is_whitespace(data[end])) end--;
  *out_data = data + start;
  *out_len  = end - start + 1u;
}

void string_util_strip_whitespace_inplace(char* data, size_t* len)
{
  size_t l = *len;
  size_t start = 0;
  while (start < l && string_util_is_whitespace(data[start])) start++;
  size_t end = l;
  while (end > start && string_util_is_whitespace(data[end - 1])) end--;
  const size_t new_len = end - start;
  if (start > 0 && new_len > 0)
    memmove(data, data + start, new_len);
  *len = new_len;
}

void string_util_ellipsise_view(small_string_t* out, const char* data, u32 len, u32 max_length, const char* ellipsis)
{
  if (!ellipsis) ellipsis = "...";
  const u32 ellipsis_len = (u32)strlen(ellipsis);
  DebugAssert(ellipsis_len > 0 && ellipsis_len <= max_length);

  if (len > max_length) {
    const u32 copy_size = u32_min(len, max_length - ellipsis_len);
    if (copy_size > 0) small_string_append_view(out, data, copy_size);
    if (copy_size != len) small_string_append_view(out, ellipsis, ellipsis_len);
  } else {
    small_string_append_view(out, data, len);
  }
}

void string_util_ellipsise_inplace(char* data, size_t* len, u32 max_length, const char* ellipsis)
{
  if (!ellipsis) ellipsis = "...";
  const u32 ellipsis_len = (u32)strlen(ellipsis);
  DebugAssert(ellipsis_len > 0 && ellipsis_len <= max_length);

  const u32 str_length = (u32)*len;
  if (str_length > max_length) {
    const u32 keep_size = u32_min(str_length, max_length - ellipsis_len);
    memcpy(data + keep_size, ellipsis, ellipsis_len);
    *len = (size_t)keep_size + ellipsis_len;
  }
}

size_t string_util_decode_hex(u8* dest, size_t dest_len, const char* data, u32 len)
{
  if ((len % 2u) != 0u) return 0;
  const size_t bytes = len / 2u;
  if (dest_len != bytes) return 0;

  for (size_t i = 0; i < bytes; i++) {
    const char hi = data[i * 2u];
    const char lo = data[i * 2u + 1u];
    if (!string_util_is_hex_digit(hi) || !string_util_is_hex_digit(lo))
      return i;
    dest[i] = (u8)((string_util_decode_hex_digit(hi) << 4) | string_util_decode_hex_digit(lo));
  }
  return bytes;
}

void string_util_encode_hex(small_string_t* out, const void* data, size_t length)
{
  small_string_append_hex(out, data, length, false);
}

void string_util_parse_fixed_hex_string(const char* str, u8* out, size_t out_len)
{
  for (size_t i = 0; i < out_len; i++) out[i] = 0;
  for (int i = 0; str[i] != '\0'; i++) {
    u8 nibble = 0;
    const char ch = str[i];
    if (ch >= '0' && ch <= '9')      nibble = (u8)(ch - '0');
    else if (ch >= 'a' && ch <= 'z') nibble = (u8)(0xA + (ch - 'a'));
    else if (ch >= 'A' && ch <= 'Z') nibble = (u8)(0xA + (ch - 'A'));

    const size_t byte_index = (size_t)i / 2u;
    if (byte_index >= out_len) break;
    out[byte_index] |= (u8)(nibble << (((i & 1) ^ 1) * 4));
  }
}

size_t string_util_encode_base64(char* dest, size_t dest_len, const u8* data, size_t data_len)
{
  static const char table[64] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V',
    'W','X','Y','Z','a','b','c','d','e','f','g','h','i','j','k','l','m','n','o','p','q','r',
    's','t','u','v','w','x','y','z','0','1','2','3','4','5','6','7','8','9','+','/' 
  };

  const size_t expected_length = string_util_encoded_base64_length(data_len);
  Assert(dest_len <= expected_length);

  size_t dest_pos = 0;
  for (size_t i = 0; i < data_len;) {
    const size_t bytes_in_sequence = size_min(data_len - i, 3);
    switch (bytes_in_sequence) {
      case 1:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[(data[i] & 3) << 4];
        dest[dest_pos++] = '=';
        dest[dest_pos++] = '=';
        break;
      case 2:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[((data[i] & 3) << 4) | ((data[i + 1] >> 4) & 15)];
        dest[dest_pos++] = table[(data[i + 1] & 15) << 2];
        dest[dest_pos++] = '=';
        break;
      case 3:
        dest[dest_pos++] = table[(data[i] >> 2) & 63];
        dest[dest_pos++] = table[((data[i] & 3) << 4) | ((data[i + 1] >> 4) & 15)];
        dest[dest_pos++] = table[((data[i + 1] & 15) << 2) | ((data[i + 2] >> 6) & 3)];
        dest[dest_pos++] = table[data[i + 2] & 63];
        break;
      default:
        UnreachableCode();
        break;
    }
    i += bytes_in_sequence;
  }

  DebugAssert(dest_pos == expected_length);
  return dest_pos;
}

size_t string_util_decode_base64(u8* dest, size_t dest_len, const char* data, u32 len)
{
  static const u8 table[128] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,52,53,54,55,
    56,57,58,59,60,61,64,64,64, 0,64,64,64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,
    13,14,15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,64,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64 
  };

  if ((len % 4u) != 0u) return 0;

  size_t data_pos = 0;
  for (u32 i = 0; i < len;) {
    const u8 byte1 = table[(u8)data[i++] & 0x7F];
    const u8 byte2 = table[(u8)data[i++] & 0x7F];
    const u8 byte3 = table[(u8)data[i++] & 0x7F];
    const u8 byte4 = table[(u8)data[i++] & 0x7F];

    if (byte1 == 64 || byte2 == 64 || byte3 == 64 || byte4 == 64)
      break;

    if (data_pos >= dest_len) break;
    dest[data_pos++] = (u8)((byte1 << 2) | (byte2 >> 4));
    if (data[i - 2] != '=' && data_pos < dest_len)
      dest[data_pos++] = (u8)((byte2 << 4) | (byte3 >> 2));
    if (data[i - 1] != '=' && data_pos < dest_len)
      dest[data_pos++] = (u8)((byte3 << 6) | byte4);
  }

  return data_pos;
}

void string_util_encode_base64_to_small_string(small_string_t* out, const u8* data, size_t data_len)
{
  const size_t encoded_len = string_util_encoded_base64_length(data_len);
  small_string_reserve(out, (u32)(small_string_length(out) + encoded_len));
  /* Append directly into the buffer to avoid an extra copy. */
  const u32 prev_len = small_string_length(out);
  small_string_resize(out, prev_len + (u32)encoded_len, '\0', false);
  const size_t written =
    string_util_encode_base64(small_string_data(out) + prev_len, encoded_len, data, data_len);
  small_string_set_size(out, prev_len + (u32)written, false);
}

bool string_util_decode_base64_alloc(u8** out_data, size_t* out_len, const char* data, u32 len)
{
  const size_t decoded_len = string_util_decoded_base64_length(data, len);
  if (decoded_len == 0) {
    *out_data = NULL;
    *out_len  = 0;
    return (len == 0);
  }
  u8* buf = (u8*)malloc(decoded_len);
  if (!buf) Panic("Memory allocation failed.");
  const size_t actual = string_util_decode_base64(buf, decoded_len, data, len);
  if (actual != decoded_len) {
    free(buf);
    *out_data = NULL;
    *out_len  = 0;
    return false;
  }
  *out_data = buf;
  *out_len  = decoded_len;
  return true;
}

size_t string_util_count_char(const char* data, u32 len, char ch)
{
  size_t n = 0;
  for (u32 i = 0; i < len; i++) n += (data[i] == ch);
  return n;
}

size_t string_util_count_char_no_case(const char* data, u32 len, char ch)
{
  const char target = ascii_lower(ch);
  size_t n = 0;
  for (u32 i = 0; i < len; i++) n += (ascii_lower(data[i]) == target);
  return n;
}

static bool split_pull_token(string_util_split_iter_t* iter, const char** out_token, u32* out_token_len)
{
  for (;;) {
    if (iter->pos > iter->len) return false;

    /* Find next delimiter from iter->pos. */
    u32 end = iter->pos;
    while (end < iter->len && iter->data[end] != iter->delimiter) end++;

    const char* raw   = iter->data + iter->pos;
    const u32   raw_n = end - iter->pos;
    iter->pos         = (end < iter->len) ? (end + 1u) : (iter->len + 1u);

    const char* tok;
    u32         tok_n;
    string_util_strip_whitespace_view(raw, raw_n, &tok, &tok_n);

    if (iter->skip_empty && tok_n == 0) {
      if (iter->pos > iter->len) return false;
      continue;
    }
    *out_token     = tok;
    *out_token_len = tok_n;
    return true;
  }
}

bool string_util_split_first(string_util_split_iter_t* iter,
                             const char* data, u32 len, char delimiter, bool skip_empty,
                             const char** out_token, u32* out_token_len)
{
  iter->data       = data;
  iter->len        = len;
  iter->pos        = 0;
  iter->delimiter  = delimiter;
  iter->skip_empty = skip_empty;
  return split_pull_token(iter, out_token, out_token_len);
}

bool string_util_split_next(string_util_split_iter_t* iter, const char** out_token, u32* out_token_len)
{
  return split_pull_token(iter, out_token, out_token_len);
}

void string_util_replace_view(small_string_t* out,
                              const char* subject, u32 subject_len,
                              const char* search,  u32 search_len,
                              const char* replacement, u32 replacement_len)
{
  small_string_clear(out);
  if (subject_len == 0 || search_len == 0) {
    small_string_append_view(out, subject, subject_len);
    return;
  }

  u32 i = 0;
  while (i + search_len <= subject_len) {
    if (memcmp(subject + i, search, search_len) == 0) {
      small_string_append_view(out, replacement, replacement_len);
      i += search_len;
    } else {
      small_string_append_char(out, subject[i]);
      i++;
    }
  }
  if (i < subject_len)
    small_string_append_view(out, subject + i, subject_len - i);
}

void string_util_replace_inplace(small_string_t* subject,
                                 const char* search, u32 search_len,
                                 const char* replacement, u32 replacement_len)
{
  if (small_string_length(subject) == 0 || search_len == 0) return;

  /* Build into a temporary then move-assign back. */
  small_string_t tmp;
  small_string_init(&tmp);
  string_util_replace_view(&tmp, small_string_c_str(subject), small_string_length(subject),
                           search, search_len, replacement, replacement_len);
  small_string_move_assign(subject, &tmp);
  small_string_destroy(&tmp);
}

void string_util_replace_char_view(small_string_t* out,
                                   const char* subject, u32 subject_len,
                                   char search, char replacement)
{
  small_string_clear(out);
  small_string_reserve(out, subject_len);
  for (u32 i = 0; i < subject_len; i++)
    small_string_append_char(out, (subject[i] == search) ? replacement : subject[i]);
}

void string_util_replace_char_inplace(small_string_t* subject, char search, char replacement)
{
  char* buf = small_string_data(subject);
  const u32 len = small_string_length(subject);
  for (u32 i = 0; i < len; i++) {
    if (buf[i] == search) buf[i] = replacement;
  }
}

bool string_util_parse_assignment_string(const char* data, u32 len,
                                         const char** out_key, u32* out_key_len,
                                         const char** out_value, u32* out_value_len)
{
  const void* eqp = memchr(data, '=', len);
  if (!eqp) {
    *out_key       = NULL;
    *out_key_len   = 0;
    *out_value     = NULL;
    *out_value_len = 0;
    return false;
  }

  const u32 pos = (u32)((const char*)eqp - data);
  string_util_strip_whitespace_view(data, pos, out_key, out_key_len);
  if (pos != (len - 1u))
    string_util_strip_whitespace_view(data + pos + 1u, len - pos - 1u, out_value, out_value_len);
  else {
    *out_value     = NULL;
    *out_value_len = 0;
  }
  return true;
}

bool string_util_get_next_token(const char** caret_data, u32* caret_len, char separator,
                                const char** out_token, u32* out_token_len)
{
  const char* d = *caret_data;
  const u32   l = *caret_len;
  const void* sep = memchr(d, separator, l);
  if (!sep) return false;

  const u32 pos = (u32)((const char*)sep - d);
  *out_token     = d;
  *out_token_len = pos;
  *caret_data    = d + pos + 1u;
  *caret_len     = l - pos - 1u;
  return true;
}

size_t string_util_get_utf8_character_count(const char* data, u32 len)
{
  size_t count = 0;
  for (size_t pos = 0; pos < len;) {
    const u8 c = (u8)data[pos];
    if (c < 0x80)                                 pos += 1;
    else if ((c & 0xE0) == 0xC0)                  pos += 2;
    else if ((c & 0xF0) == 0xE0)                  pos += 3;
    else if ((c & 0xF8) == 0xF0 && c <= 0xF4)     pos += 4;
    else                                          pos += 1;
    ++count;
  }
  return count;
}

void string_util_encode_and_append_utf8(small_string_t* out, char32_t ch)
{
  if (ch <= 0x7F) {
    small_string_append_char(out, (char)(u8)ch);
  } else if (ch <= 0x07FF) {
    small_string_append_char(out, (char)(u8)(0xC0u | (u8)((ch >> 6) & 0x1Fu)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)( ch       & 0x3Fu)));
  } else if (ch <= 0xFFFF) {
    small_string_append_char(out, (char)(u8)(0xE0u | (u8)((ch >> 12) & 0x0Fu)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)((ch >>  6) & 0x3Fu)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)( ch        & 0x3Fu)));
  } else if (ch <= 0x10FFFF) {
    small_string_append_char(out, (char)(u8)(0xF0u | (u8)((ch >> 18) & 0x07u)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)((ch >> 12) & 0x3Fu)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)((ch >>  6) & 0x3Fu)));
    small_string_append_char(out, (char)(u8)(0x80u | (u8)( ch        & 0x3Fu)));
  } else {
    small_string_append_char(out, (char)0xEFu);
    small_string_append_char(out, (char)0xBFu);
    small_string_append_char(out, (char)0xBDu);
  }
}

size_t string_util_get_encoded_utf8_length(char32_t ch)
{
  if (ch <= 0x7F)     return 1;
  if (ch <= 0x07FF)   return 2;
  if (ch <= 0xFFFF)   return 3;
  if (ch <= 0x10FFFF) return 4;
  return 3;
}

size_t string_util_encode_and_append_utf8_buffer(void* utf8, size_t pos, size_t size, char32_t ch)
{
  u8* utf8_bytes = (u8*)utf8 + pos;
  if (ch <= 0x7F) {
    if (pos == size) return 0;
    utf8_bytes[0] = (u8)ch;
    return 1;
  } else if (ch <= 0x07FF) {
    if ((pos + 1) >= size) return 0;
    utf8_bytes[0] = (u8)(0xC0u | (u8)((ch >> 6) & 0x1Fu));
    utf8_bytes[1] = (u8)(0x80u | (u8)( ch       & 0x3Fu));
    return 2;
  } else if (ch <= 0xFFFF) {
    if ((pos + 3) >= size) return 0;
    utf8_bytes[0] = (u8)(0xE0u | (u8)((ch >> 12) & 0x0Fu));
    utf8_bytes[1] = (u8)(0x80u | (u8)((ch >>  6) & 0x3Fu));
    utf8_bytes[2] = (u8)(0x80u | (u8)( ch        & 0x3Fu));
    return 3;
  } else if (ch <= 0x10FFFF) {
    if ((pos + 4) >= size) return 0;
    utf8_bytes[0] = (u8)(0xF0u | (u8)((ch >> 18) & 0x07u));
    utf8_bytes[1] = (u8)(0x80u | (u8)((ch >> 12) & 0x3Fu));
    utf8_bytes[2] = (u8)(0x80u | (u8)((ch >>  6) & 0x3Fu));
    utf8_bytes[3] = (u8)(0x80u | (u8)( ch        & 0x3Fu));
    return 4;
  } else {
    if ((pos + 3) >= size) return 0;
    utf8_bytes[0] = 0xEFu;
    utf8_bytes[1] = 0xBFu;
    utf8_bytes[2] = 0xBDu;
    return 3;
  }
}

size_t string_util_decode_utf8(const void* bytes, size_t length, char32_t* ch)
{
  const u8* s = (const u8*)bytes;
  if (length == 0) {
    *ch = STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER;
    return 1;
  }
  if (s[0] < 0x80) {
    *ch = s[0];
    return 1;
  } else if ((s[0] & 0xE0) == 0xC0) {
    if (length < 2) goto invalid;
    *ch = (char32_t)(((u32)(s[0] & 0x1F) << 6) | ((u32)(s[1] & 0x3F)));
    return 2;
  } else if ((s[0] & 0xF0) == 0xE0) {
    if (length < 3) goto invalid;
    *ch = (char32_t)(((u32)(s[0] & 0x0F) << 12) | ((u32)(s[1] & 0x3F) << 6) | ((u32)(s[2] & 0x3F)));
    return 3;
  } else if ((s[0] & 0xF8) == 0xF0 && (s[0] <= 0xF4)) {
    if (length < 4) goto invalid;
    *ch = (char32_t)(((u32)(s[0] & 0x07) << 18) | ((u32)(s[1] & 0x3F) << 12) |
                     ((u32)(s[2] & 0x3F) <<  6) |  (u32)(s[3] & 0x3F));
    return 4;
  }

invalid:
  *ch = STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER;
  return 1;
}

size_t string_util_decode_utf8_view(const char* data, u32 len, size_t offset, char32_t* ch)
{
  return string_util_decode_utf8(data + offset, (size_t)len - offset, ch);
}

size_t string_util_encode_and_append_utf16(void* utf16, size_t pos, size_t size, char32_t codepoint)
{
  u8* const utf16_bytes = (u8*)utf16 + (pos * sizeof(u16));
  if (codepoint <= 0xFFFF) {
    if (pos == size) return 0;
    /* Surrogates are invalid in scalar form. */
    const u16 codepoint16 =
      (u16)((codepoint >= 0xD800 && codepoint <= 0xDFFF) ? STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER : codepoint);
    memcpy(utf16_bytes, &codepoint16, sizeof(codepoint16));
    return 1;
  } else if (codepoint <= 0x10FFFF) {
    if ((pos + 1) >= size) return 0;
    codepoint -= 0x010000u;
    const u16 low  = (u16)((((u32)codepoint >> 10) & 0x3FFu) + 0xD800u);
    const u16 high = (u16)(((u32)codepoint        & 0x3FFu) + 0xDC00u);
    memcpy(utf16_bytes,             &low,  sizeof(low));
    memcpy(utf16_bytes + sizeof(u16), &high, sizeof(high));
    return 2;
  } else {
    const u16 value = (u16)STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER;
    memcpy(utf16_bytes, &value, sizeof(value));
    return 1;
  }
}

static size_t decode_utf16_impl(const void* bytes, size_t pos, size_t size, char32_t* ch, bool swap)
{
  const u8* const utf16_bytes = (const u8*)bytes + pos * sizeof(u16);

  u16 high;
  memcpy(&high, utf16_bytes, sizeof(high));
  if (swap) high = ByteSwap_u16(high);

  if (high >= 0xD800 && high <= 0xDBFF) {
    if ((size - pos) < 2) {
      *ch = STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER;
      return 1;
    }

    u16 low;
    memcpy(&low, utf16_bytes + sizeof(u16), sizeof(low));
    if (swap) low = ByteSwap_u16(low);

    if (low >= 0xDC00 && low <= 0xDFFF) {
      *ch = (char32_t)((((u32)high - 0xD800u) << 10) + ((u32)low - 0xDC00u) + 0x10000u);
      return 2;
    }
    *ch = STRING_UTIL_UNICODE_REPLACEMENT_CHARACTER;
    return 2;
  }

  *ch = (char32_t)high;
  return 1;
}

size_t string_util_decode_utf16(const void* bytes, size_t pos, size_t size, char32_t* codepoint)
{
  return decode_utf16_impl(bytes, pos, size, codepoint, false);
}

size_t string_util_decode_utf16_be(const void* bytes, size_t pos, size_t size, char32_t* codepoint)
{
  return decode_utf16_impl(bytes, pos, size, codepoint, true);
}

static void decode_utf16_string_impl(small_string_t* out, const void* bytes, size_t size, bool swap)
{
  const size_t u16_size = size / 2u;
  for (size_t pos = 0; pos < u16_size;) {
    char32_t codepoint;
    const size_t units = decode_utf16_impl(bytes, pos, u16_size, &codepoint, swap);
    string_util_encode_and_append_utf8(out, codepoint);
    pos += units;
  }
}

void string_util_decode_utf16_string(small_string_t* out, const void* bytes, size_t size)
{
  decode_utf16_string_impl(out, bytes, size, false);
}

void string_util_decode_utf16_be_string(small_string_t* out, const void* bytes, size_t size)
{
  decode_utf16_string_impl(out, bytes, size, true);
}

bool string_util_byte_pattern_search(const u8* bytes, size_t bytes_len,
                                     const char* pattern, u32 pattern_len,
                                     size_t* out_offset)
{
  /* First pass: count active hex/wildcard pairs. */
  size_t mask_bytes = 0;
  bool   hinibble   = true;
  for (u32 i = 0; i < pattern_len; i++) {
    const char p = pattern[i];
    if ((p >= '0' && p <= '9') || (p >= 'a' && p <= 'f') || (p >= 'A' && p <= 'F') || p == '?') {
      hinibble = !hinibble;
      if (hinibble) mask_bytes++;
    } else if (p == ' ' || p == '\r' || p == '\n') {
      continue;
    } else {
      break;
    }
  }
  if (mask_bytes == 0) return false;

  /* Pattern can't possibly match if longer than the haystack. */
  if (mask_bytes > bytes_len) return false;

  /* Single allocation: [match_bytes ... match_masks]. */
  u8* buf = (u8*)malloc(mask_bytes * 2u);
  if (!buf) Panic("Memory allocation failed.");
  u8* match_bytes_arr = buf;
  u8* match_masks_arr = buf + mask_bytes;

  hinibble       = true;
  u8 match_byte  = 0;
  u8 match_mask  = 0;
  size_t mlen    = 0;
  for (u32 i = 0; i < pattern_len; i++) {
    const char p = pattern[i];
    u8 nibble = 0, nibble_mask = 0xF;
    if      (p >= '0' && p <= '9') nibble = (u8)(p - '0');
    else if (p >= 'a' && p <= 'f') nibble = (u8)(p - 'a' + 0xA);
    else if (p >= 'A' && p <= 'F') nibble = (u8)(p - 'A' + 0xA);
    else if (p == '?')             nibble_mask = 0;
    else if (p == ' ' || p == '\r' || p == '\n') continue;
    else break;

    hinibble = !hinibble;
    if (hinibble) {
      match_bytes_arr[mlen] = (u8)(nibble | (match_byte << 4));
      match_masks_arr[mlen] = (u8)(nibble_mask | (match_mask << 4));
      mlen++;
    } else {
      match_byte = nibble;
      match_mask = nibble_mask;
    }
  }
  DebugAssert(mlen == mask_bytes);

  bool   found      = false;
  size_t found_off  = 0;
  const size_t max_search_offset = bytes_len - mask_bytes;
  for (size_t offset = 0; offset < max_search_offset; offset++) {
    const u8* start = bytes + offset;
    size_t mo = 0;
    for (;;) {
      if ((start[mo] & match_masks_arr[mo]) != match_bytes_arr[mo])
        break;
      mo++;
      if (mo == mask_bytes) {
        /* Faithful to original: keep scanning to last match. */
        found     = true;
        found_off = offset;
        break;
      }
    }
  }

  free(buf);

  if (found) *out_offset = found_off;
  return found;
}

void string_util_stride_mem_cpy(void* dst, size_t dst_stride,
                                const void* src, size_t src_stride,
                                size_t copy_size, size_t count)
{
  if (src_stride == dst_stride && src_stride == copy_size) {
    memcpy(dst, src, src_stride * count);
    return;
  }
  const u8* src_ptr = (const u8*)src;
  u8*       dst_ptr = (u8*)dst;
  for (size_t i = 0; i < count; i++) {
    memcpy(dst_ptr, src_ptr, copy_size);
    src_ptr += src_stride;
    dst_ptr += dst_stride;
  }
}

int string_util_stride_mem_cmp(const void* p1, size_t p1_stride,
                               const void* p2, size_t p2_stride,
                               size_t copy_size, size_t count)
{
  if (p1_stride == p2_stride && p1_stride == copy_size)
    return memcmp(p1, p2, p1_stride * count);

  const u8* p1_ptr = (const u8*)p1;
  const u8* p2_ptr = (const u8*)p2;
  for (size_t i = 0; i < count; i++) {
    int result = memcmp(p1_ptr, p2_ptr, copy_size);
    if (result != 0) return result;
    p1_ptr += p1_stride;
    p2_ptr += p2_stride;
  }
  return 0;
}
