/*
 * Hand-rolled CUE parser. CUE files are a flat sequence of commands:
 *
 *   FILE "name" BINARY|WAVE
 *   TRACK NN MODE
 *   INDEX NN MM:SS:FF
 *   PREGAP MM:SS:FF
 *   FLAGS PRE|DCP|4CH|SCMS
 *   REM ...           (comment)
 *
 * No regex needed. We strip the leading command keyword, then pull
 * whitespace-separated tokens (with quoted-string support) out of the rest.
 */

#include "cue_parser.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"
#include "common/string_util.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_CHANNEL(CueParser);

static char* dup_view(const char* data, u32 len)
{
  char* p = (char*)malloc((size_t)len + 1u);
  if (!p) Panic("Memory allocation failed.");
  if (len > 0) memcpy(p, data, len);
  p[len] = '\0';
  return p;
}

static bool token_match(const char* data, u32 len, const char* keyword)
{
  const size_t klen = strlen(keyword);
  if ((size_t)len != klen) return false;
  return strncasecmp(data, keyword, klen) == 0;
}

/* Pulls one whitespace-separated token (with optional double-quoted string)
 * out of *line. Advances *line past the token. Returns false on empty/EOL. */
static bool get_token(const char** line, const char** out_data, u32* out_len)
{
  const char* p = *line;
  while (*p != '\0' && string_util_is_whitespace(*p))
    p++;
  if (*p == '\0') {
    *out_data = NULL;
    *out_len  = 0;
    return false;
  }

  const char* start;
  const char* end;
  if (*p == '"') {
    p++;
    start = p;
    while (*p != '\0' && *p != '"') p++;
    if (*p != '"') {
      *line = p;
      *out_data = NULL;
      *out_len  = 0;
      return false;
    }
    end = p;
    p++; /* eat closing quote */
  } else {
    start = p;
    while (*p != '\0' && !string_util_is_whitespace(*p)) p++;
    end = p;
  }

  *line     = p;
  *out_data = start;
  *out_len  = (u32)(end - start);
  return true;
}

/* Parses MM:SS:FF where MM is unbounded, SS in [0,59], FF in [0,74]. */
static bool parse_msf(const char* data, u32 len, cd_image_position_t* out)
{
  static const s32 max_values[3] = { INT32_MAX, 59, 74 };
  u32 parts[3] = { 0, 0, 0 };
  u32 part = 0;
  u32 start = 0;

  /* Loop tolerates a buggy condition encountered in the wild. */
  for (;;) {
    while (start < len && (data[start] < '0' || data[start] > '9'))
      start++;
    if (start == len)
      return false;

    u32 end = start;
    while (end < len && data[end] >= '0' && data[end] <= '9')
      end++;

    s32 value = 0;
    if (!string_util_from_chars_s32(data + start, end - start, 10, &value, NULL))
      return false;
    if (value < 0 || value > max_values[part])
      return false;

    parts[part++] = (u32)value;
    if (part == 3)
      break;

    while (end < len && string_util_is_whitespace(data[end]))
      end++;
    if (end == len || data[end] != ':')
      return false;
    start = end + 1u;
  }

  out->minute = (u8)parts[0];
  out->second = (u8)parts[1];
  out->frame  = (u8)parts[2];
  return true;
}

PRINTFLIKE(3, 4) static void set_error(u32 line_number, Error* error, const char* fmt, ...)
{
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  ERROR_LOG("Cue parse error at line %u: %s", line_number, buf);
  Error_set_string_fmt(error, "Cue parse error at line %u: %s", line_number, buf);
}

const cd_image_position_t* cue_parser_track_get_index(const cue_parser_track_t* t, u32 n)
{
  for (u32 k = 0; k < t->index_count; k++) {
    if (t->indices[k].number == n)
      return &t->indices[k].msf;
  }
  return NULL;
}

static void track_push_index(cue_parser_track_t* t, u32 number, cd_image_position_t msf)
{
  if (t->index_count == t->index_capacity) {
    const u32 new_cap = (t->index_capacity == 0) ? 4u : (t->index_capacity * 2u);
    cue_parser_index_t* p = (cue_parser_index_t*)realloc(t->indices, new_cap * sizeof(cue_parser_index_t));
    if (!p) Panic("Memory allocation failed.");
    t->indices = p;
    t->index_capacity = new_cap;
  }
  t->indices[t->index_count].number = number;
  t->indices[t->index_count].msf    = msf;
  t->index_count++;
}

static void track_init(cue_parser_track_t* t)
{
  memset(t, 0, sizeof(*t));
}

static void track_destroy(cue_parser_track_t* t)
{
  free(t->file);
  free(t->indices);
  memset(t, 0, sizeof(*t));
}

void cue_parser_file_init(cue_parser_file_t* f)
{
  memset(f, 0, sizeof(*f));
}

void cue_parser_file_destroy(cue_parser_file_t* f)
{
  for (u32 k = 0; k < f->track_count; k++)
    track_destroy(&f->tracks[k]);
  free(f->tracks);
  free(f->current_file);
  if (f->has_current_track)
    track_destroy(&f->current_track);
  memset(f, 0, sizeof(*f));
}

const cue_parser_track_t* cue_parser_file_get_track(const cue_parser_file_t* f, u32 number)
{
  for (u32 k = 0; k < f->track_count; k++) {
    if (f->tracks[k].number == number)
      return &f->tracks[k];
  }
  return NULL;
}

static cue_parser_track_t* file_get_mutable_track(cue_parser_file_t* f, u32 number)
{
  for (u32 k = 0; k < f->track_count; k++) {
    if (f->tracks[k].number == number)
      return &f->tracks[k];
  }
  return NULL;
}

static cue_parser_track_t* file_push_track(cue_parser_file_t* f)
{
  if (f->track_count == f->track_capacity) {
    const u32 new_cap = (f->track_capacity == 0) ? 4u : (f->track_capacity * 2u);
    cue_parser_track_t* p = (cue_parser_track_t*)realloc(f->tracks, new_cap * sizeof(cue_parser_track_t));
    if (!p) Panic("Memory allocation failed.");
    f->tracks = p;
    f->track_capacity = new_cap;
  }
  cue_parser_track_t* tr = &f->tracks[f->track_count++];
  track_init(tr);
  return tr;
}

static bool handle_file(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  const char* fname_data; u32 fname_len;
  const char* mode_data;  u32 mode_len;
  if (!get_token(&line, &fname_data, &fname_len)) {
    set_error(line_number, error, "Missing filename");
    return false;
  }
  get_token(&line, &mode_data, &mode_len);

  cue_parser_file_format_t format;
  if (token_match(mode_data, mode_len, "BINARY"))      format = CUE_PARSER_FILE_FORMAT_BINARY;
  else if (token_match(mode_data, mode_len, "WAVE"))   format = CUE_PARSER_FILE_FORMAT_WAVE;
  else {
    set_error(line_number, error, "Only BINARY and WAVE modes are supported");
    return false;
  }

  free(f->current_file);
  f->current_file        = dup_view(fname_data, fname_len);
  f->current_file_format = format;
  f->has_current_file    = true;
  DEBUG_LOG("File '%s'", f->current_file);
  return true;
}

static bool complete_last_track(cue_parser_file_t* f, u32 line_number, Error* error)
{
  if (!f->has_current_track)
    return true;

  cue_parser_track_t* ct = &f->current_track;

  const cd_image_position_t* index1 = cue_parser_track_get_index(ct, 1);
  if (!index1) {
    set_error(line_number, error, "Track %u is missing index 1", (u32)ct->number);
    return false;
  }

  /* Indices must be monotonically increasing. */
  for (u32 k = 0; k < ct->index_count; k++) {
    const u32 n = ct->indices[k].number;
    if (n == 0) continue;
    const cd_image_position_t* prev = cue_parser_track_get_index(ct, n - 1u);
    if (prev && cd_image_position_gt(*prev, ct->indices[k].msf)) {
      set_error(line_number, error, "Index %u is after index %u in track %u",
                n - 1u, n, (u32)ct->number);
      return false;
    }
  }

  const cd_image_position_t* index0 = cue_parser_track_get_index(ct, 0);
  if (index0 && ct->has_zero_pregap) {
    WARNING_LOG("Zero pregap and index 0 specified in track %u, ignoring zero pregap", (u32)ct->number);
    ct->has_zero_pregap = false;
  }

  ct->start = *index1;

  /* Move into the tracks vector (transfer heap ownership). */
  cue_parser_track_t* slot = file_push_track(f);
  *slot = *ct;
  memset(ct, 0, sizeof(*ct));
  f->has_current_track = false;
  return true;
}

static bool handle_track(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  if (!complete_last_track(f, line_number, error))
    return false;

  if (!f->has_current_file) {
    set_error(line_number, error, "Starting a track declaration without a file set");
    return false;
  }

  const char* num_data; u32 num_len;
  if (!get_token(&line, &num_data, &num_len)) {
    set_error(line_number, error, "Missing track number");
    return false;
  }
  s32 track_number = 0;
  if (!string_util_from_chars_s32(num_data, num_len, 10, &track_number, NULL) ||
      track_number < CUE_PARSER_MIN_TRACK_NUMBER || track_number > CUE_PARSER_MAX_TRACK_NUMBER) {
    set_error(line_number, error, "Invalid track number %d", track_number);
    return false;
  }

  const char* mode_data; u32 mode_len;
  get_token(&line, &mode_data, &mode_len);

  cd_image_track_mode_t mode;
  if      (token_match(mode_data, mode_len, "AUDIO"))      mode = CD_IMAGE_TRACK_MODE_AUDIO;
  else if (token_match(mode_data, mode_len, "MODE1/2048")) mode = CD_IMAGE_TRACK_MODE_MODE1;
  else if (token_match(mode_data, mode_len, "MODE1/2352")) mode = CD_IMAGE_TRACK_MODE_MODE1_RAW;
  else if (token_match(mode_data, mode_len, "MODE2/2336")) mode = CD_IMAGE_TRACK_MODE_MODE2;
  else if (token_match(mode_data, mode_len, "MODE2/2048")) mode = CD_IMAGE_TRACK_MODE_MODE2_FORM1;
  else if (token_match(mode_data, mode_len, "MODE2/2342")) mode = CD_IMAGE_TRACK_MODE_MODE2_FORM2;
  else if (token_match(mode_data, mode_len, "MODE2/2332")) mode = CD_IMAGE_TRACK_MODE_MODE2_FORM_MIX;
  else if (token_match(mode_data, mode_len, "MODE2/2352")) mode = CD_IMAGE_TRACK_MODE_MODE2_RAW;
  else {
    set_error(line_number, error, "Invalid mode: '%.*s'", (int)mode_len, mode_data);
    return false;
  }

  track_init(&f->current_track);
  f->current_track.number      = (u8)track_number;
  f->current_track.file        = dup_view(f->current_file, (u32)strlen(f->current_file));
  f->current_track.file_format = f->current_file_format;
  f->current_track.mode        = mode;
  f->has_current_track         = true;
  return true;
}

static bool handle_index(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  if (!f->has_current_track) {
    set_error(line_number, error, "Setting index without track");
    return false;
  }

  const char* num_data; u32 num_len;
  if (!get_token(&line, &num_data, &num_len)) {
    set_error(line_number, error, "Missing index number");
    return false;
  }
  s32 index_number = -1;
  if (!string_util_from_chars_s32(num_data, num_len, 10, &index_number, NULL) ||
      index_number < CUE_PARSER_MIN_INDEX_NUMBER || index_number > CUE_PARSER_MAX_INDEX_NUMBER) {
    set_error(line_number, error, "Invalid index number %d", index_number);
    return false;
  }

  if (cue_parser_track_get_index(&f->current_track, (u32)index_number) != NULL) {
    set_error(line_number, error, "Duplicate index %d", index_number);
    return false;
  }

  const char* msf_data; u32 msf_len;
  if (!get_token(&line, &msf_data, &msf_len)) {
    set_error(line_number, error, "Missing index location");
    return false;
  }
  cd_image_position_t msf;
  if (!parse_msf(msf_data, msf_len, &msf)) {
    set_error(line_number, error, "Invalid index location '%.*s'", (int)msf_len, msf_data);
    return false;
  }

  track_push_index(&f->current_track, (u32)index_number, msf);
  return true;
}

static bool handle_pregap(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  if (!f->has_current_track) {
    set_error(line_number, error, "Setting pregap without track");
    return false;
  }
  if (f->current_track.has_zero_pregap) {
    set_error(line_number, error, "Pregap already specified for track %u", (u32)f->current_track.number);
    return false;
  }

  const char* msf_data; u32 msf_len;
  if (!get_token(&line, &msf_data, &msf_len)) {
    set_error(line_number, error, "Missing pregap location");
    return false;
  }
  cd_image_position_t msf;
  if (!parse_msf(msf_data, msf_len, &msf)) {
    set_error(line_number, error, "Invalid pregap location '%.*s'", (int)msf_len, msf_data);
    return false;
  }
  f->current_track.zero_pregap     = msf;
  f->current_track.has_zero_pregap = true;
  return true;
}

static bool handle_flags(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  if (!f->has_current_track) {
    set_error(line_number, error, "Flags command outside of track");
    return false;
  }

  for (;;) {
    const char* tok_data; u32 tok_len;
    if (!get_token(&line, &tok_data, &tok_len))
      break;
    if      (token_match(tok_data, tok_len, "PRE"))  f->current_track.flags |= CUE_PARSER_TRACK_FLAG_PRE_EMPHASIS;
    else if (token_match(tok_data, tok_len, "DCP"))  f->current_track.flags |= CUE_PARSER_TRACK_FLAG_COPY_PERMITTED;
    else if (token_match(tok_data, tok_len, "4CH"))  f->current_track.flags |= CUE_PARSER_TRACK_FLAG_FOUR_CHANNEL_AUDIO;
    else if (token_match(tok_data, tok_len, "SCMS")) f->current_track.flags |= CUE_PARSER_TRACK_FLAG_SERIAL_COPY_MGMT;
    else WARNING_LOG("Unknown track flag '%.*s'", (int)tok_len, tok_data);
  }
  return true;
}

static bool parse_line(cue_parser_file_t* f, const char* line, u32 line_number, Error* error)
{
  const char* cmd_data; u32 cmd_len;
  if (!get_token(&line, &cmd_data, &cmd_len))
    return true;

  if (token_match(cmd_data, cmd_len, "REM"))
    return true;

  if (token_match(cmd_data, cmd_len, "FILE"))   return handle_file  (f, line, line_number, error);
  if (token_match(cmd_data, cmd_len, "TRACK"))  return handle_track (f, line, line_number, error);
  if (token_match(cmd_data, cmd_len, "INDEX"))  return handle_index (f, line, line_number, error);
  if (token_match(cmd_data, cmd_len, "PREGAP")) return handle_pregap(f, line, line_number, error);
  if (token_match(cmd_data, cmd_len, "FLAGS"))  return handle_flags (f, line, line_number, error);

  if (token_match(cmd_data, cmd_len, "POSTGAP")) {
    WARNING_LOG("Ignoring '%.*s' command", (int)cmd_len, cmd_data);
    return true;
  }

  /* Definitely-ignored commands. */
  static const char* const ignored[] = {
    "CATALOG", "CDTEXTFILE", "ISRC", "TRACK_ISRC", "TITLE", "PERFORMER", "SONGWRITER", "COMPOSER",
    "ARRANGER", "MESSAGE", "DISC_ID", "GENRE", "TOC_INFO1", "TOC_INFO2", "UPC_EAN", "SIZE_INFO", 
  };
  for (size_t k = 0; k < sizeof(ignored) / sizeof(ignored[0]); k++) {
    if (token_match(cmd_data, cmd_len, ignored[k]))
      return true;
  }

  set_error(line_number, error, "Invalid command '%.*s'", (int)cmd_len, cmd_data);
  return false;
}

static bool set_track_lengths(cue_parser_file_t* f, u32 line_number, Error* error)
{
  for (u32 k = 0; k < f->track_count; k++) {
    const cue_parser_track_t* t = &f->tracks[k];
    if (t->number <= 1u)
      continue;
    cue_parser_track_t* prev = file_get_mutable_track(f, (u32)t->number - 1u);
    if (!prev || !prev->file || !t->file || strcmp(prev->file, t->file) != 0)
      continue;

    if (cd_image_position_gt(prev->start, t->start)) {
      set_error(line_number, error, "Track %u start greater than track %u start",
                (u32)prev->number, (u32)t->number);
      return false;
    }

    /* Use index 0, otherwise index 1. */
    const cd_image_position_t* start_idx = cue_parser_track_get_index(t, 0);
    if (!start_idx) start_idx = cue_parser_track_get_index(t, 1);

    prev->length     = cd_image_position_from_lba(cd_image_position_to_lba(*start_idx) -
                                                  cd_image_position_to_lba(prev->start));
    prev->has_length = true;
  }
  return true;
}

bool cue_parser_file_parse_fp(cue_parser_file_t* f, FILE* fp, Error* error)
{
  char line[1024];
  u32 line_number = 1;
  while (fgets(line, sizeof(line), fp)) {
    if (!parse_line(f, line, line_number, error))
      return false;
    line_number++;
  }
  if (!complete_last_track(f, line_number, error))
    return false;
  if (!set_track_lengths(f, line_number, error))
    return false;
  return true;
}

bool cue_parser_file_parse_buffer(cue_parser_file_t* f, const char* buffer, Error* error)
{
  u32 line_number = 1;
  const char* p = buffer;

  /* Walk one line at a time, NUL-terminating each segment in a stack copy. */
  while (*p != '\0') {
    const char* eol = p;
    while (*eol != '\0' && *eol != '\n') eol++;

    char tmp[1024];
    size_t len = (size_t)(eol - p);
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1u;
    memcpy(tmp, p, len);
    /* Drop trailing CR for CRLF lines. */
    if (len > 0 && tmp[len - 1u] == '\r') len--;
    tmp[len] = '\0';

    if (!parse_line(f, tmp, line_number, error))
      return false;

    line_number++;
    p = (*eol == '\0') ? eol : (eol + 1);
  }

  if (!complete_last_track(f, line_number, error))
    return false;
  if (!set_track_lengths(f, line_number, error))
    return false;
  return true;
}
