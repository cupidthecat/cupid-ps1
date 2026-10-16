/*
 * CDImageCCD: CloneCD .ccd descriptor + .img raw 2352-byte sector data +
 * .sub 96-byte raw interleaved subchannel triple.
 *
 * The .ccd is a Windows-INI-style text file with [Section] / Key=Value
 * lines.  Sections used: [CloneCD] (header check), [Disc] (TocEntries +
 * lead-out), [Entry N] (per-TOC point: Point, ADR, Control, PLBA),
 * [TRACK N] (per-track: MODE, INDEX 0, INDEX 1).  Comments start with ';'.
 *
 * INI parser is a single linear pass; all entries land in one flat array
 * of (section, key, value) triples - section-aware lookup is a strcasecmp
 * scan, fine for the ~200 lines a CCD typically holds.  get_int_value
 * parses with strtol(base=0).
 */

#include "cd_image.h"

#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/small_string.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

LOG_CHANNEL(CDImage);

#define CCD_IMG_SECTOR_SIZE   ((u32)CD_IMAGE_RAW_SECTOR_SIZE)
#define CCD_SUBCHANNEL_BYTES  ((u32)CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME)
#define CCD_ALL_SUBCODE_BYTES ((u32)CD_IMAGE_ALL_SUBCODE_SIZE)

typedef struct cd_image_ccd {
  cd_image_t base;        /* MUST be first */
  FILE*      img_file;
  FILE*      sub_file;
  s64        img_position;
} cd_image_ccd_t;

typedef struct ccd_kv {
  char* section;
  char* key;
  char* value;
} ccd_kv_t;

typedef struct ccd_kv_list {
  ccd_kv_t* items;
  size_t    count;
  size_t    cap;
} ccd_kv_list_t;

static const char* basename_view(const char* path)
{
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static char* dup_range(const char* data, size_t len)
{
  char* out = (char*)malloc(len + 1);
  if (!out) abort();
  memcpy(out, data, len);
  out[len] = '\0';
  return out;
}

static void kv_list_init(ccd_kv_list_t* l) { l->items = NULL; l->count = 0; l->cap = 0; }

static void kv_list_destroy(ccd_kv_list_t* l)
{
  for (size_t i = 0; i < l->count; i++) {
    free(l->items[i].section);
    free(l->items[i].key);
    free(l->items[i].value);
  }
  free(l->items);
  l->items = NULL;
  l->count = 0;
  l->cap = 0;
}

static void kv_list_push(ccd_kv_list_t* l, const char* section, const char* key, const char* key_end,
                         const char* value, const char* value_end)
{
  if (l->count == l->cap) {
    size_t new_cap = l->cap ? l->cap * 2 : 64;
    l->items = (ccd_kv_t*)realloc(l->items, new_cap * sizeof(*l->items));
    if (!l->items) abort();
    l->cap = new_cap;
  }
  l->items[l->count].section = section ? dup_range(section, strlen(section)) : dup_range("", 0);
  l->items[l->count].key     = dup_range(key, (size_t)(key_end - key));
  l->items[l->count].value   = dup_range(value, (size_t)(value_end - value));
  l->count++;
}

/* Strip leading + trailing whitespace from [data, end). */
static void strip_ws(const char** pdata, const char** pend)
{
  const char* d = *pdata;
  const char* e = *pend;
  while (d < e && isspace((u8)*d))   d++;
  while (e > d && isspace((u8)e[-1])) e--;
  *pdata = d;
  *pend  = e;
}

/* Walk the CCD text, populating kv_list.  Returns false on syntax issues
 * but currently only `false` for OOM (handled via abort). */
static void parse_ini_text(const char* text, size_t text_len, ccd_kv_list_t* out)
{
  small_string_t section;
  small_string_init(&section);

  const char* p = text;
  const char* end = text + text_len;
  while (p < end) {
    /* find end of line */
    const char* line_end = (const char*)memchr(p, '\n', (size_t)(end - p));
    if (!line_end) line_end = end;

    const char* line = p;
    const char* line_end_strip = line_end;
    /* strip CR, then strip whitespace */
    if (line_end_strip > line && line_end_strip[-1] == '\r') line_end_strip--;
    strip_ws(&line, &line_end_strip);

    if (line < line_end_strip && *line != ';') {
      if (*line == '[' && line_end_strip[-1] == ']') {
        const char* sec_data = line + 1;
        const char* sec_end  = line_end_strip - 1;
        small_string_clear(&section);
        small_string_append_view(&section, sec_data, (u32)(sec_end - sec_data));
      } else {
        const char* eq = (const char*)memchr(line, '=', (size_t)(line_end_strip - line));
        if (eq) {
          const char* key = line;
          const char* key_end = eq;
          const char* val = eq + 1;
          const char* val_end = line_end_strip;
          strip_ws(&key, &key_end);
          strip_ws(&val, &val_end);
          if (key < key_end) {
            kv_list_push(out, small_string_c_str(&section), key, key_end, val, val_end);
          }
        }
      }
    }

    p = (line_end < end) ? line_end + 1 : end;
  }

  small_string_destroy(&section);
}

static bool kv_list_has_section(const ccd_kv_list_t* l, const char* section)
{
  for (size_t i = 0; i < l->count; i++) {
    if (strcasecmp(l->items[i].section, section) == 0)
      return true;
  }
  return false;
}

static bool kv_list_get_int(const ccd_kv_list_t* l, const char* section, const char* key, s32* out)
{
  for (size_t i = 0; i < l->count; i++) {
    if (strcasecmp(l->items[i].section, section) == 0 && strcasecmp(l->items[i].key, key) == 0) {
      char* endp = NULL;
      errno = 0;
      long v = strtol(l->items[i].value, &endp, 0);
      if (errno != 0 || endp == l->items[i].value)
        return false;
      *out = (s32)v;
      return true;
    }
  }
  return false;
}

typedef struct ccd_track_info {
  u32                   track_number;
  cd_image_track_mode_t mode;
  s32                   index0;   /* -1 if not present */
  s32                   index1;
  u8                    control;
} ccd_track_info_t;

static int track_info_cmp(const void* a, const void* b)
{
  const ccd_track_info_t* ta = (const ccd_track_info_t*)a;
  const ccd_track_info_t* tb = (const ccd_track_info_t*)b;
  if (ta->track_number < tb->track_number) return -1;
  if (ta->track_number > tb->track_number) return  1;
  return 0;
}

static bool cdimage_ccd_read_sector_from_index(cd_image_t* self, void* buffer,
                                               const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_ccd_t* img = (cd_image_ccd_t*)self;
  const s64 file_position = (s64)(index->file_offset + ((u64)lba_in_index * (u64)CCD_IMG_SECTOR_SIZE));
  if (img->img_position != file_position) {
    if (fseek(img->img_file, (long)file_position, SEEK_SET) != 0)
      return false;
    img->img_position = file_position;
  }
  if (fread(buffer, CCD_IMG_SECTOR_SIZE, 1, img->img_file) != 1) {
    fseek(img->img_file, (long)img->img_position, SEEK_SET);
    return false;
  }
  img->img_position += CCD_IMG_SECTOR_SIZE;
  return true;
}

static bool cdimage_ccd_read_subchannel_q(cd_image_t* self, cd_image_subq_t* subq,
                                          const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_ccd_t* img = (cd_image_ccd_t*)self;

  /* Virtual pregaps (file_sector_size==0) → generated Q. */
  if (index->is_pregap && index->file_sector_size == 0) {
    cd_image_generate_subq_from_index(self, subq, index, lba_in_index);
    return true;
  }

  /* Q is the second 12-byte block of the 96-byte subcode. */
  const u64 q_offset = CCD_SUBCHANNEL_BYTES;
  const s64 sub_offset = (s64)(
      ((index->file_offset / CCD_IMG_SECTOR_SIZE) * CCD_ALL_SUBCODE_BYTES)
      + ((u64)lba_in_index * CCD_ALL_SUBCODE_BYTES) + q_offset);

  if (fseek(img->sub_file, (long)sub_offset, SEEK_SET) != 0
      || fread(subq, CCD_SUBCHANNEL_BYTES, 1, img->sub_file) != 1) {
    WARNING_LOG("Failed to read subq for sector %u", (u32)(index->start_lba_on_disc + lba_in_index));
    cd_image_generate_subq_from_index(self, subq, index, lba_in_index);
    return true;
  }
  return true;
}

static bool cdimage_ccd_has_subchannel_data(const cd_image_t* self) { (void)self; return true; }

static s64 cdimage_ccd_get_size_on_disk(const cd_image_t* self)
{
  cd_image_ccd_t* img = (cd_image_ccd_t*)self;
  s64 sz = fs_fsize64(img->img_file, NULL);
  if (sz < 0) sz = 0;
  if (img->sub_file) {
    s64 ss = fs_fsize64(img->sub_file, NULL);
    if (ss > 0) sz += ss;
  }
  return sz;
}

static void cdimage_ccd_destroy(cd_image_t* self)
{
  cd_image_ccd_t* img = (cd_image_ccd_t*)self;
  if (img->sub_file) { fclose(img->sub_file); img->sub_file = NULL; }
  if (img->img_file) { fclose(img->img_file); img->img_file = NULL; }
}

static const cd_image_vtable_t s_ccd_vtable = {
   .read_sector_from_index = cdimage_ccd_read_sector_from_index,
  .read_subchannel_q      = cdimage_ccd_read_subchannel_q,
  .has_subchannel_data    = cdimage_ccd_has_subchannel_data,
  .get_size_on_disk       = cdimage_ccd_get_size_on_disk,
  .destroy                = cdimage_ccd_destroy, 
};

static cd_image_ccd_t* alloc_ccd_image(void)
{
  cd_image_ccd_t* img = (cd_image_ccd_t*)malloc(sizeof(*img));
  if (!img) abort();
  cd_image_init(&img->base, &s_ccd_vtable);
  img->img_file = NULL;
  img->sub_file = NULL;
  img->img_position = 0;
  return img;
}

static bool open_and_parse_ccd(cd_image_ccd_t* img, const char* filename, Error* error)
{
  /* 1. Read CCD as text. */
  char*  text = NULL;
  size_t text_len = 0;
  if (!fs_read_file_to_string_path(filename, &text, &text_len, error)) {
    Error_set_string_fmt(error, "Failed to open ccd '%s'", basename_view(filename));
    return false;
  }

  /* 2. Open .img + .sub siblings. */
  small_string_t img_path, sub_path;
  small_string_init(&img_path);
  small_string_init(&sub_path);
  path_change_extension_cstr(&img_path, filename, "img");
  path_change_extension_cstr(&sub_path, filename, "sub");

  img->img_file = fs_open_shared_file(small_string_c_str(&img_path), "rb",
                                      FS_FILE_SHARE_DENY_WRITE, error);
  if (!img->img_file) {
    Error_set_string_fmt(error, "Failed to open img file '%s'", basename_view(small_string_c_str(&img_path)));
    small_string_destroy(&img_path);
    small_string_destroy(&sub_path);
    free(text);
    return false;
  }
  img->sub_file = fs_open_shared_file(small_string_c_str(&sub_path), "rb",
                                      FS_FILE_SHARE_DENY_WRITE, error);
  if (!img->sub_file) {
    Error_set_string_fmt(error, "Failed to open sub file '%s'", basename_view(small_string_c_str(&sub_path)));
    small_string_destroy(&img_path);
    small_string_destroy(&sub_path);
    free(text);
    return false;
  }
  small_string_destroy(&img_path);
  small_string_destroy(&sub_path);

  /* 3. Parse INI sections + verify [CloneCD] / [Disc] headers + TocEntries. */
  ccd_kv_list_t kv;
  kv_list_init(&kv);
  parse_ini_text(text, text_len, &kv);
  free(text);

  if (!kv_list_has_section(&kv, "CloneCD")) {
    ERROR_LOG("Missing [CloneCD] header in '%s'", basename_view(filename));
    Error_set_string_fmt(error, "Missing [CloneCD] header in '%s'", basename_view(filename));
    kv_list_destroy(&kv);
    return false;
  }
  if (!kv_list_has_section(&kv, "Disc")) {
    ERROR_LOG("Missing [Disc] section in '%s'", basename_view(filename));
    Error_set_string_fmt(error, "Missing [Disc] section in '%s'", basename_view(filename));
    kv_list_destroy(&kv);
    return false;
  }

  s32 toc_entries = 0;
  if (!kv_list_get_int(&kv, "Disc", "TocEntries", &toc_entries) || toc_entries < 3) {
    ERROR_LOG("Invalid or missing TocEntries in '%s'", basename_view(filename));
    Error_set_string_fmt(error, "Invalid or missing TocEntries in '%s'", basename_view(filename));
    kv_list_destroy(&kv);
    return false;
  }

  /* 4. Walk Entry N for tracks 1..99 + lead-out (point 0xA2). */
  cd_image_lba_t leadout_lba = 0;
  ccd_track_info_t parsed_tracks[100];
  size_t parsed_tracks_count = 0;

  for (s32 i = 0; i < toc_entries; i++) {
    char section[32];
    snprintf(section, sizeof(section), "Entry %d", i);
    s32 point = 0;
    if (!kv_list_get_int(&kv, section, "Point", &point))
      continue;

    s32 plba = 0; bool has_plba = kv_list_get_int(&kv, section, "PLBA", &plba);
    s32 control = 0; bool has_control = kv_list_get_int(&kv, section, "Control", &control);

    const u8 point_val = (u8)point;
    if (point_val == 0xA2 && has_plba) {
      leadout_lba = (cd_image_lba_t)plba;
    } else if (point_val >= 1 && point_val <= 99) {
      ccd_track_info_t info;
      info.track_number = point_val;
      info.control      = has_control ? (u8)control : 0u;

      char track_section[32];
      snprintf(track_section, sizeof(track_section), "TRACK %u", info.track_number);
      s32 mode_val = 0;
      if (kv_list_get_int(&kv, track_section, "MODE", &mode_val)) {
        switch (mode_val) {
          case 0:  info.mode = CD_IMAGE_TRACK_MODE_AUDIO;     break;
          case 1:  info.mode = CD_IMAGE_TRACK_MODE_MODE1_RAW; break;
          case 2:
          default: info.mode = CD_IMAGE_TRACK_MODE_MODE2_RAW; break;
        }
      } else {
        info.mode = (info.control & 0x04u) ? CD_IMAGE_TRACK_MODE_MODE2_RAW : CD_IMAGE_TRACK_MODE_AUDIO;
      }

      s32 idx0 = 0, idx1 = 0;
      info.index0 = kv_list_get_int(&kv, track_section, "INDEX 0", &idx0) ? idx0 : -1;
      info.index1 = kv_list_get_int(&kv, track_section, "INDEX 1", &idx1) ? idx1 : (has_plba ? plba : 0);

      if (parsed_tracks_count < (sizeof(parsed_tracks) / sizeof(parsed_tracks[0]))) {
        parsed_tracks[parsed_tracks_count++] = info;
      }
    }
  }

  kv_list_destroy(&kv);

  if (parsed_tracks_count == 0) {
    ERROR_LOG("File '%s' contains no track entries", basename_view(filename));
    Error_set_string_fmt(error, "File '%s' contains no track entries", basename_view(filename));
    return false;
  }

  /* If lead-out missing, derive from .img file size. */
  if (leadout_lba == 0) {
    s64 img_size = fs_fsize64(img->img_file, NULL);
    if (img_size > 0) {
      leadout_lba = (cd_image_lba_t)(img_size / CCD_IMG_SECTOR_SIZE);
    } else {
      ERROR_LOG("Could not determine lead-out position in '%s'", basename_view(filename));
      Error_set_string_fmt(error, "Could not determine lead-out position in '%s'", basename_view(filename));
      return false;
    }
  }

  /* 5. Sort tracks by number + verify dense from 1. */
  qsort(parsed_tracks, parsed_tracks_count, sizeof(parsed_tracks[0]), track_info_cmp);
  if (parsed_tracks[0].track_number != 1) {
    ERROR_LOG("File '%s' must contain a track 1", basename_view(filename));
    Error_set_string_fmt(error, "File '%s' must contain a track 1", basename_view(filename));
    return false;
  }
  for (size_t i = 1; i < parsed_tracks_count; i++) {
    if (parsed_tracks[i].track_number != parsed_tracks[i - 1].track_number + 1) {
      const u32 missing = parsed_tracks[i - 1].track_number + 1;
      ERROR_LOG("File '%s' has missing track number %u", basename_view(filename), missing);
      Error_set_string_fmt(error, "File '%s' has missing track number %u", basename_view(filename), missing);
      return false;
    }
  }

  /* Track 1 implicit pregap offset. */
  const cd_image_lba_t plba_offset = (parsed_tracks[0].index0 >= 0) ? 0u : 150u;

  /* 6. Emit indices + tracks. */
  for (size_t i = 0; i < parsed_tracks_count; i++) {
    const ccd_track_info_t* ti = &parsed_tracks[i];
    const cd_image_lba_t track_index0 = (cd_image_lba_t)((ti->index0 >= 0) ? ti->index0 : ti->index1);
    const cd_image_lba_t track_index1 = (cd_image_lba_t)ti->index1;

    cd_image_lba_t next_track_index0, next_track_index1;
    if (i + 1 < parsed_tracks_count) {
      const ccd_track_info_t* next = &parsed_tracks[i + 1];
      next_track_index0 = (cd_image_lba_t)((next->index0 >= 0) ? next->index0 : next->index1);
      next_track_index1 = (cd_image_lba_t)next->index1;
    } else {
      next_track_index0 = leadout_lba;
      next_track_index1 = leadout_lba;
    }

    if (track_index1 < track_index0 || next_track_index0 <= track_index1 || next_track_index0 > next_track_index1) {
      ERROR_LOG("Track %u has invalid length (start %u/%u, next %u/%u)", ti->track_number,
                track_index0, track_index1, next_track_index0, next_track_index1);
      Error_set_string_fmt(error, "Track %u has invalid length", ti->track_number);
      return false;
    }

    cd_image_subq_control_t control = { 0 };
    cd_image_subq_control_set_data(&control, ti->mode != CD_IMAGE_TRACK_MODE_AUDIO);

    u32 toc_track_length = next_track_index0 - track_index0;

    if (track_index1 > track_index0) {
      /* Pregap is in the IMG file. */
      const cd_image_lba_t pregap_length = track_index1 - track_index0;
      cd_image_index_t pregap = { 0 };
      pregap.start_lba_on_disc  = (cd_image_lba_t)ti->index0 + plba_offset;
      pregap.start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_length);
      pregap.length             = pregap_length;
      pregap.track_number       = ti->track_number;
      pregap.index_number       = 0;
      pregap.file_index         = 0;
      pregap.file_sector_size   = CCD_IMG_SECTOR_SIZE;
      pregap.file_offset        = (u64)ti->index0 * CCD_IMG_SECTOR_SIZE;
      pregap.mode               = ti->mode;
      pregap.submode            = CD_IMAGE_SUBCHANNEL_MODE_RAW;
      pregap.control            = control;
      pregap.is_pregap          = true;
      *cd_image_push_index(&img->base) = pregap;
    } else if (ti->track_number == 1 && plba_offset > 0) {
      /* Implicit synthesised track-1 pregap. */
      cd_image_index_t pregap = { 0 };
      pregap.start_lba_on_disc  = 0;
      pregap.start_lba_in_track = (cd_image_lba_t)(-(s32)plba_offset);
      pregap.length             = plba_offset;
      pregap.track_number       = ti->track_number;
      pregap.index_number       = 0;
      pregap.mode               = ti->mode;
      pregap.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      pregap.control            = control;
      pregap.is_pregap          = true;
      *cd_image_push_index(&img->base) = pregap;
      toc_track_length += plba_offset;
    }

    cd_image_track_t* tr = cd_image_push_track(&img->base);
    tr->track_number = ti->track_number;
    tr->start_lba    = track_index1 + plba_offset;
    tr->first_index  = img->base.index_count;
    tr->length       = toc_track_length;
    tr->mode         = ti->mode;
    tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
    tr->control      = control;

    cd_image_index_t data_idx = { 0 };
    data_idx.start_lba_on_disc  = track_index1 + plba_offset;
    data_idx.start_lba_in_track = 0;
    data_idx.track_number       = ti->track_number;
    data_idx.index_number       = 1;
    data_idx.file_index         = 0;
    data_idx.file_sector_size   = CCD_IMG_SECTOR_SIZE;
    data_idx.file_offset        = (u64)track_index1 * CCD_IMG_SECTOR_SIZE;
    data_idx.mode               = ti->mode;
    data_idx.submode            = CD_IMAGE_SUBCHANNEL_MODE_RAW;
    data_idx.control            = control;
    data_idx.is_pregap          = false;
    data_idx.length             = next_track_index0 - track_index1;
    *cd_image_push_index(&img->base) = data_idx;
  }

  if (img->base.track_count == 0) {
    ERROR_LOG("File '%s' contains no tracks", basename_view(filename));
    Error_set_string_fmt(error, "File '%s' contains no tracks", basename_view(filename));
    return false;
  }

  const cd_image_track_t* last = &img->base.tracks[img->base.track_count - 1];
  img->base.lba_count = last->start_lba + last->length;
  cd_image_add_lead_out_index(&img->base);

  return cd_image_seek_track_msf(&img->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

cd_image_t* cd_image_open_ccd(const char* path, Error* error)
{
  cd_image_ccd_t* img = alloc_ccd_image();
  if (!open_and_parse_ccd(img, path, error)) {
    cd_image_destroy(&img->base);
    return NULL;
  }
  return &img->base;
}
