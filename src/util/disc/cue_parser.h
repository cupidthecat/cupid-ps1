/*
 * CueParser::File becomes cue_parser_file_t.  Tracks and indices are stored
 * in growable heap arrays. std::regex is not used - the parser is a plain
 * line-oriented hand-rolled tokenizer (CUE is whitespace/quote separated).
 *
 * Lifecycle:
 *   cue_parser_file_t f; cue_parser_file_init(&f);
 *   bool ok = cue_parser_file_parse_fp(&f, fp, err);
 *   ...
 *   cue_parser_file_destroy(&f);
 */

#ifndef CUPID_UTIL_CUE_PARSER_H
#define CUPID_UTIL_CUE_PARSER_H

#include "cd_image.h"

#include "common/types.h"

#include <stdio.h>

typedef struct Error Error;

enum {
  CUE_PARSER_MIN_TRACK_NUMBER = 1,
  CUE_PARSER_MAX_TRACK_NUMBER = 99,
  CUE_PARSER_MIN_INDEX_NUMBER = 0,
  CUE_PARSER_MAX_INDEX_NUMBER = 99,
};

typedef enum {
  CUE_PARSER_TRACK_FLAG_PRE_EMPHASIS         = (1u << 0),
  CUE_PARSER_TRACK_FLAG_COPY_PERMITTED       = (1u << 1),
  CUE_PARSER_TRACK_FLAG_FOUR_CHANNEL_AUDIO   = (1u << 2),
  CUE_PARSER_TRACK_FLAG_SERIAL_COPY_MGMT     = (1u << 3),
} cue_parser_track_flag_t;

typedef enum {
  CUE_PARSER_FILE_FORMAT_BINARY = 0,
  CUE_PARSER_FILE_FORMAT_WAVE   = 1,
} cue_parser_file_format_t;

typedef struct {
  u32                 number;
  cd_image_position_t msf;
} cue_parser_index_t;

typedef struct {
  u8                       number;
  u8                       flags;
  cd_image_track_mode_t    mode;
  cue_parser_file_format_t file_format;
  char*                    file;          /* heap, owned */

  cue_parser_index_t* indices;
  u32                 index_count;
  u32                 index_capacity;

  cd_image_position_t start;

  bool                has_length;
  cd_image_position_t length;

  bool                has_zero_pregap;
  cd_image_position_t zero_pregap;
} cue_parser_track_t;

typedef struct {
  cue_parser_track_t* tracks;
  u32                 track_count;
  u32                 track_capacity;

  /* Most recent FILE statement. */
  bool                       has_current_file;
  char*                      current_file;          /* heap, owned */
  cue_parser_file_format_t   current_file_format;

  /* Track currently being assembled (between TRACK and the next TRACK/EOF). */
  bool               has_current_track;
  cue_parser_track_t current_track;
} cue_parser_file_t;

void cue_parser_file_init   (cue_parser_file_t* f);
void cue_parser_file_destroy(cue_parser_file_t* f);

/* Parses an opened file pointer (at offset 0). The fp is read until EOF;
 * caller still owns/closes the FILE*. */
bool cue_parser_file_parse_fp(cue_parser_file_t* f, FILE* fp, Error* error);

/* Parses a NUL-terminated buffer (line-by-line). */
bool cue_parser_file_parse_buffer(cue_parser_file_t* f, const char* buffer, Error* error);

/* Returns NULL when no track with that number was parsed. */
const cue_parser_track_t* cue_parser_file_get_track(const cue_parser_file_t* f, u32 number);

/* Index lookup on a track. */
const cd_image_position_t* cue_parser_track_get_index(const cue_parser_track_t* t, u32 n);

ALWAYS_INLINE bool cue_parser_track_has_flag(const cue_parser_track_t* t, cue_parser_track_flag_t flag)
{
  return (t->flags & (u8)flag) != 0;
}

#endif /* CUPID_UTIL_CUE_PARSER_H */
