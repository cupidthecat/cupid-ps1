/*
 * Shift-JIS to UTF-8 conversion.  Used for PS1 game metadata, BIOS strings
 * two raw helpers; we additionally provide a small_string_t-based API and a
 * single-codepoint decoder for callers that want to walk a buffer.
 */

#ifndef CUPID_UTIL_SHIFTJIS_H
#define CUPID_UTIL_SHIFTJIS_H

#include "common/small_string.h"
#include "common/types.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void  shiftjis_sjis2ascii(char* b_data);
char* shiftjis_sjis2utf8 (char* input);

void shiftjis_to_utf8(const u8* in, size_t in_len, small_string_t* out);

/* shiftjis_decode_codepoint: decode a single Shift-JIS character starting at
 *   in[0]; on success writes the Unicode codepoint to *codepoint and the
 *   number of source bytes consumed (1 or 2) to *consumed, and returns true.
 *   Returns false if the buffer is empty or a two-byte lead is not followed
 *   by a trail byte. */

bool shiftjis_decode_codepoint(const u8* in, size_t in_len,
                               size_t* consumed, u32* codepoint);

#ifdef __cplusplus
}
#endif

#endif /* CUPID_UTIL_SHIFTJIS_H */
