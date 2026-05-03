/*
 * Win32 / Darwin / IOKit dropped.
 *
 * CDImageDeviceLinux: opens a real CD-ROM device file (/dev/sr*, /dev/cdrom)
 * via O_RDONLY + ioctl/SG_IO.  Tries SCSI READ CD with full subcode first;
 * falls back to SubQ-only, then CDROMREADRAW, then SCSI raw.
 *
 * GetDeviceList enumerates block-class devices with ID_CDROM=1 via libudev.
 * Both functions are reachable only when the user provides a /dev path or
 * a future CLI flag invokes the listing - they're cold paths.
 *
 * No `Core::GetBaseBoolSettingValue("CDROM", "IgnoreHostSubcode", false)`
 * equivalent in cupid-ps1; subcode reads are attempted unconditionally.
 */

#include "cd_image.h"

#include "common/error.h"
#include "common/log.h"
#include "common/path.h"

#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <linux/cdrom.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <unistd.h>

LOG_CHANNEL(CDImage);

#define DEVICE_SCSI_CMD_LENGTH      12
#define DEVICE_RAW_SECTOR_SIZE      ((u32)CD_IMAGE_RAW_SECTOR_SIZE)
#define DEVICE_ALL_SUBCODE_SIZE     ((u32)CD_IMAGE_ALL_SUBCODE_SIZE)
#define DEVICE_SUBCHANNEL_BYTES     ((u32)CD_IMAGE_SUBCHANNEL_BYTES_PER_FRAME)
#define DEVICE_FRAMES_PER_SECOND    ((u32)CD_IMAGE_FRAMES_PER_SECOND)
#define DEVICE_RAW_READ_OFFSET      (DEVICE_FRAMES_PER_SECOND * 2u)
#define DEVICE_BUFFER_SIZE          (DEVICE_RAW_SECTOR_SIZE + DEVICE_ALL_SUBCODE_SIZE)
#define DEVICE_MAX_TRACK_NUMBER     99u

typedef enum {
  SCSI_READ_MODE_NONE      = 0,
  SCSI_READ_MODE_RAW       = 1,
  SCSI_READ_MODE_FULL      = 2,
  SCSI_READ_MODE_SUBQ_ONLY = 3,
} scsi_read_mode_t;

typedef struct cd_image_device {
  cd_image_t       base;            /* MUST be first */
  int              fd;
  cd_image_lba_t   current_lba;
  scsi_read_mode_t read_mode;
  u8               buffer[DEVICE_BUFFER_SIZE];
} cd_image_device_t;

static void fill_scsi_read_command(u8 cmd[DEVICE_SCSI_CMD_LENGTH], u32 sector, scsi_read_mode_t mode)
{
  cmd[0] = 0xBE;                      /* READ CD */
  cmd[1] = 0x00;
  cmd[2] = (u8)(sector >> 24);
  cmd[3] = (u8)(sector >> 16);
  cmd[4] = (u8)(sector >> 8);
  cmd[5] = (u8)sector;
  cmd[6] = 0x00;
  cmd[7] = 0x00;
  cmd[8] = 0x01;
  cmd[9] = (u8)((1 << 7) | (0x3 << 5) | (1 << 4) | (1 << 3) | (0 << 2));
  if (mode == SCSI_READ_MODE_NONE || mode == SCSI_READ_MODE_RAW) cmd[10] = 0x0;
  else if (mode == SCSI_READ_MODE_FULL)                          cmd[10] = 0x1;
  else                                                           cmd[10] = 0x2;
  cmd[11] = 0;
}

static void fill_scsi_set_speed_command(u8 cmd[DEVICE_SCSI_CMD_LENGTH], u32 speed)
{
  cmd[0] = 0xDA;
  cmd[1] = 0x00;
  cmd[2] = (u8)(speed - 1u);
  for (int i = 3; i < DEVICE_SCSI_CMD_LENGTH; i++) cmd[i] = 0;
}

static u32 scsi_read_command_output_size(scsi_read_mode_t mode)
{
  switch (mode) {
    case SCSI_READ_MODE_NONE:
    case SCSI_READ_MODE_RAW:       return DEVICE_RAW_SECTOR_SIZE;
    case SCSI_READ_MODE_FULL:      return DEVICE_RAW_SECTOR_SIZE + DEVICE_ALL_SUBCODE_SIZE;
    case SCSI_READ_MODE_SUBQ_ONLY: return DEVICE_RAW_SECTOR_SIZE + DEVICE_SUBCHANNEL_BYTES;
  }
  return 0;
}

static bool verify_scsi_read_data(const u8* buf, u32 size, scsi_read_mode_t mode, cd_image_lba_t expected_sector)
{
  const u32 expected_size = scsi_read_command_output_size(mode);
  if (size != expected_size) {
    ERROR_LOG("SCSI returned %u bytes, expected %u", size, expected_size);
    return false;
  }
  const cd_image_position_t expected_pos = cd_image_position_from_lba(expected_sector);

  if (mode == SCSI_READ_MODE_FULL) {
    u8 deinterleaved[DEVICE_ALL_SUBCODE_SIZE];
    cd_image_subq_t subq;
    cd_image_deinterleave_subcode(buf + DEVICE_RAW_SECTOR_SIZE, deinterleaved);
    memcpy(&subq, &deinterleaved[DEVICE_SUBCHANNEL_BYTES], sizeof(subq));
    if (!cd_image_subq_is_crc_valid(&subq)) {
      WARNING_LOG("SCSI full subcode read returned invalid SubQ CRC");
      return false;
    }
    const cd_image_position_t got = cd_image_position_from_bcd(
        subq.absolute_minute_bcd, subq.absolute_second_bcd, subq.absolute_frame_bcd);
    if (!cd_image_position_eq(expected_pos, got)) {
      WARNING_LOG("SCSI full subcode read returned invalid MSF");
      return false;
    }
    return true;
  } else if (mode == SCSI_READ_MODE_SUBQ_ONLY) {
    cd_image_subq_t subq;
    memcpy(&subq, buf + DEVICE_RAW_SECTOR_SIZE, sizeof(subq));
    if (!cd_image_subq_is_crc_valid(&subq)) {
      WARNING_LOG("SCSI subq read returned invalid SubQ CRC");
      return false;
    }
    const cd_image_position_t got = cd_image_position_from_bcd(
        subq.absolute_minute_bcd, subq.absolute_second_bcd, subq.absolute_frame_bcd);
    if (!cd_image_position_eq(expected_pos, got)) {
      WARNING_LOG("SCSI subq read returned invalid MSF");
      return false;
    }
    return true;
  }
  /* NONE/RAW: nothing meaningful to validate. */
  return true;
}

static bool do_scsi_command(cd_image_device_t* d, u8 cmd[DEVICE_SCSI_CMD_LENGTH], u8* out_buf, u32 out_size, u32* out_actual)
{
  sg_io_hdr_t hdr;
  memset(&hdr, 0, sizeof(hdr));
  hdr.cmd_len = DEVICE_SCSI_CMD_LENGTH;
  hdr.interface_id = 'S';
  hdr.dxfer_direction = (out_size == 0) ? SG_DXFER_NONE : SG_DXFER_FROM_DEV;
  hdr.mx_sb_len = 0;
  hdr.dxfer_len = out_size;
  hdr.dxferp = (out_size == 0) ? NULL : out_buf;
  hdr.cmdp = cmd;
  hdr.timeout = 10000;

  if (ioctl(d->fd, SG_IO, &hdr) != 0) {
    ERROR_LOG("ioctl(SG_IO) for command 0x%02X failed: %d", cmd[0], errno);
    return false;
  }
  if (hdr.status != 0) {
    ERROR_LOG("SCSI command 0x%02X failed with status %u", cmd[0], hdr.status);
    return false;
  }
  if (out_actual) *out_actual = (u32)hdr.dxfer_len;
  return true;
}

static bool do_scsi_read(cd_image_device_t* d, cd_image_lba_t lba, scsi_read_mode_t mode, u32* out_size)
{
  u8 cmd[DEVICE_SCSI_CMD_LENGTH];
  fill_scsi_read_command(cmd, lba, mode);
  const u32 size = scsi_read_command_output_size(mode);
  return do_scsi_command(d, cmd, d->buffer, size, out_size);
}

static bool do_set_speed(cd_image_device_t* d, u32 speed_multiplier)
{
  u8 cmd[DEVICE_SCSI_CMD_LENGTH];
  fill_scsi_set_speed_command(cmd, speed_multiplier);
  u32 actual;
  return do_scsi_command(d, cmd, NULL, 0, &actual);
}

static bool do_raw_read(cd_image_device_t* d, cd_image_lba_t lba)
{
  cd_image_position_t msf = cd_image_position_from_lba(lba + DEVICE_RAW_READ_OFFSET);
  /* CDROMREADRAW: the buffer's first 3 bytes are the {minute, second, frame}
   * MSF target written in BIN format (matches the cd_image_position_t struct's
   * {minute, second, frame} layout). */
  d->buffer[0] = msf.minute;
  d->buffer[1] = msf.second;
  d->buffer[2] = msf.frame;
  if (ioctl(d->fd, CDROMREADRAW, d->buffer) != 0) {
    ERROR_LOG("CDROMREADRAW for LBA %u failed: %d", lba, errno);
    return false;
  }
  return true;
}

static bool read_sector_to_buffer(cd_image_device_t* d, cd_image_lba_t lba)
{
  if (d->read_mode != SCSI_READ_MODE_NONE) {
    u32 size = 0;
    if (!do_scsi_read(d, lba, d->read_mode, &size)) return false;
    const u32 expected = scsi_read_command_output_size(d->read_mode);
    if (size != expected) {
      ERROR_LOG("Read of LBA %u failed: only got %u of %u bytes", lba, size, expected);
      return false;
    }
  } else {
    if (!do_raw_read(d, lba)) return false;
  }
  d->current_lba = lba;
  return true;
}

static bool determine_read_mode(cd_image_device_t* d, Error* error)
{
  (void)error;
  const cd_image_lba_t track_1_lba = (cd_image_lba_t)(d->base.indices[d->base.tracks[0].first_index].file_offset);
  const cd_image_lba_t track_1_subq_lba = track_1_lba + DEVICE_FRAMES_PER_SECOND * 2u;
  u32 size = 0;

  if (do_scsi_read(d, track_1_lba, SCSI_READ_MODE_FULL, &size)) {
    if (verify_scsi_read_data(d->buffer, size, SCSI_READ_MODE_FULL, track_1_subq_lba)) {
      VERBOSE_LOG("Using SCSI reads with subcode");
      d->read_mode = SCSI_READ_MODE_FULL;
      return true;
    }
  }
  WARNING_LOG("Full subcode failed, trying SCSI subq...");
  if (do_scsi_read(d, track_1_lba, SCSI_READ_MODE_SUBQ_ONLY, &size)) {
    if (verify_scsi_read_data(d->buffer, size, SCSI_READ_MODE_SUBQ_ONLY, track_1_subq_lba)) {
      VERBOSE_LOG("Using SCSI reads with subq only");
      d->read_mode = SCSI_READ_MODE_SUBQ_ONLY;
      return true;
    }
  }
  WARNING_LOG("SCSI subcode failed, trying CDROMREADRAW...");
  if (do_raw_read(d, track_1_lba)) {
    WARNING_LOG("Using CDROMREADRAW; libcrypt games will not run correctly");
    d->read_mode = SCSI_READ_MODE_NONE;
    return true;
  }
  WARNING_LOG("CDROMREADRAW failed, trying SCSI without subcode...");
  if (do_scsi_read(d, track_1_lba, SCSI_READ_MODE_RAW, &size)) {
    if (verify_scsi_read_data(d->buffer, size, SCSI_READ_MODE_RAW, track_1_subq_lba)) {
      WARNING_LOG("Using SCSI raw reads; libcrypt games will not run correctly");
      d->read_mode = SCSI_READ_MODE_RAW;
      return true;
    }
  }
  ERROR_LOG("No read modes were successful, cannot use device.");
  return false;
}

static bool cdimage_device_read_sector_from_index(cd_image_t* self, void* buffer,
                                                  const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_device_t* d = (cd_image_device_t*)self;
  if (index->file_sector_size == 0) return false;
  const cd_image_lba_t disc_lba = (cd_image_lba_t)index->file_offset + lba_in_index;
  if (d->current_lba != disc_lba && !read_sector_to_buffer(d, disc_lba)) return false;
  memcpy(buffer, d->buffer, DEVICE_RAW_SECTOR_SIZE);
  return true;
}

static bool cdimage_device_read_subchannel_q(cd_image_t* self, cd_image_subq_t* subq,
                                             const cd_image_index_t* index, cd_image_lba_t lba_in_index)
{
  cd_image_device_t* d = (cd_image_device_t*)self;
  if (index->file_sector_size == 0 || d->read_mode < SCSI_READ_MODE_FULL) {
    cd_image_generate_subq_from_index(self, subq, index, lba_in_index);
    return true;
  }

  const cd_image_lba_t disc_lba = (cd_image_lba_t)index->file_offset + lba_in_index;
  if (d->current_lba != disc_lba && !read_sector_to_buffer(d, disc_lba)) return false;

  if (d->read_mode == SCSI_READ_MODE_SUBQ_ONLY) {
    memcpy(subq, d->buffer + DEVICE_RAW_SECTOR_SIZE, DEVICE_SUBCHANNEL_BYTES);
    return true;
  }
  /* SCSI_READ_MODE_FULL: deinterleave and pull out P,Q. */
  u8 deinterleaved[DEVICE_ALL_SUBCODE_SIZE];
  cd_image_deinterleave_subcode(d->buffer + DEVICE_RAW_SECTOR_SIZE, deinterleaved);
  memcpy(subq, deinterleaved + DEVICE_SUBCHANNEL_BYTES, DEVICE_SUBCHANNEL_BYTES);
  return true;
}

static bool cdimage_device_has_subchannel_data(const cd_image_t* self)
{
  const cd_image_device_t* d = (const cd_image_device_t*)self;
  return d->read_mode >= SCSI_READ_MODE_FULL;
}

static void cdimage_device_destroy(cd_image_t* self)
{
  cd_image_device_t* d = (cd_image_device_t*)self;
  if (d->fd >= 0) { close(d->fd); d->fd = -1; }
}

static const cd_image_vtable_t s_device_vtable = {
   .read_sector_from_index = cdimage_device_read_sector_from_index,
  .read_subchannel_q      = cdimage_device_read_subchannel_q,
  .has_subchannel_data    = cdimage_device_has_subchannel_data,
  .destroy                = cdimage_device_destroy, 
};

static cd_image_device_t* alloc_device_image(void)
{
  cd_image_device_t* d = (cd_image_device_t*)malloc(sizeof(*d));
  if (!d) abort();
  cd_image_init(&d->base, &s_device_vtable);
  d->fd = -1;
  d->current_lba = (cd_image_lba_t)~(cd_image_lba_t)0;
  d->read_mode = SCSI_READ_MODE_NONE;
  memset(d->buffer, 0, sizeof(d->buffer));
  return d;
}

static bool open_and_parse_device(cd_image_device_t* d, const char* filename, Error* error)
{
  d->fd = open(filename, O_RDONLY);
  if (d->fd < 0) {
    Error_set_errno_prefix(error, "Failed to open device: ", errno);
    return false;
  }

  /* 4x speed.  Best-effort, swallow failure. */
  const int read_speed = 4;
  if (!do_set_speed(d, (u32)read_speed) && ioctl(d->fd, CDROM_SELECT_SPEED, &read_speed) != 0) {
    WARNING_LOG("ioctl(CDROM_SELECT_SPEED) failed: %d", errno);
  }

  struct cdrom_tochdr toc_hdr;
  memset(&toc_hdr, 0, sizeof(toc_hdr));
  if (ioctl(d->fd, CDROMREADTOCHDR, &toc_hdr) != 0) {
    Error_set_errno_prefix(error, "ioctl(CDROMREADTOCHDR) failed: ", errno);
    return false;
  }
  if (toc_hdr.cdth_trk1 < toc_hdr.cdth_trk0) {
    Error_set_string_fmt(error, "Last track %u is before first track %u", toc_hdr.cdth_trk1, toc_hdr.cdth_trk0);
    return false;
  }

  struct cdrom_tocentry toc_ent;
  memset(&toc_ent, 0, sizeof(toc_ent));
  toc_ent.cdte_format = CDROM_LBA;

  cd_image_lba_t disc_lba = 0;
  int last_track_lba = 0;
  const u32 num_tracks = (u32)((toc_hdr.cdth_trk1 - toc_hdr.cdth_trk0) + 1);

  for (u32 i = 0; i < num_tracks; i++) {
    const u32 track_num = (u32)toc_hdr.cdth_trk0 + i;
    toc_ent.cdte_track = (u8)track_num;
    if (ioctl(d->fd, CDROMREADTOCENTRY, &toc_ent) < 0) {
      Error_set_errno_prefix(error, "ioctl(CDROMREADTOCENTRY) failed: ", errno);
      return false;
    }

    if (d->base.track_count > 0) {
      cd_image_track_t* prev_track = &d->base.tracks[d->base.track_count - 1];
      cd_image_index_t* prev_index = &d->base.indices[d->base.index_count - 1];
      if (track_num < prev_track->track_number) {
        ERROR_LOG("Invalid TOC, track %u less than %u", track_num, prev_track->track_number);
        return false;
      }
      const cd_image_lba_t prev_len = (cd_image_lba_t)(toc_ent.cdte_addr.lba - last_track_lba);
      prev_track->length += prev_len;
      prev_index->length += prev_len;
      disc_lba += prev_len;
    }
    last_track_lba = toc_ent.cdte_addr.lba;

    cd_image_subq_control_t control = { 0 };
    control.bits = (u8)(toc_ent.cdte_adr | (toc_ent.cdte_ctrl << 4));
    const cd_image_lba_t track_lba = (cd_image_lba_t)toc_ent.cdte_addr.lba;
    const cd_image_track_mode_t track_mode = cd_image_subq_control_data(control)
        ? CD_IMAGE_TRACK_MODE_MODE2_RAW : CD_IMAGE_TRACK_MODE_AUDIO;

    /* Synth pregap on first track only (TODO: handle other tracks). */
    const u32 pregap_frames = (i == 0) ? 150u : 0u;
    if (pregap_frames > 0) {
      cd_image_index_t pregap = { 0 };
      pregap.start_lba_on_disc  = disc_lba;
      pregap.start_lba_in_track = (cd_image_lba_t)(-(s32)pregap_frames);
      pregap.length             = pregap_frames;
      pregap.track_number       = track_num;
      pregap.index_number       = 0;
      pregap.mode               = track_mode;
      pregap.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      pregap.control            = control;
      pregap.is_pregap          = true;
      *cd_image_push_index(&d->base) = pregap;
      disc_lba += pregap_frames;
    }

    if (track_num <= DEVICE_MAX_TRACK_NUMBER) {
      cd_image_track_t* tr = cd_image_push_track(&d->base);
      tr->track_number = track_num;
      tr->start_lba    = disc_lba;
      tr->first_index  = d->base.index_count;
      tr->length       = 0;
      tr->mode         = track_mode;
      tr->submode      = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      tr->control      = control;

      cd_image_index_t idx1 = { 0 };
      idx1.start_lba_on_disc  = disc_lba;
      idx1.start_lba_in_track = 0;
      idx1.length             = 0;
      idx1.track_number       = track_num;
      idx1.index_number       = 1;
      idx1.file_index         = 0;
      idx1.file_sector_size   = DEVICE_RAW_SECTOR_SIZE;
      idx1.file_offset        = (u64)track_lba;
      idx1.mode               = track_mode;
      idx1.submode            = CD_IMAGE_SUBCHANNEL_MODE_NONE;
      idx1.control            = control;
      idx1.is_pregap          = false;
      *cd_image_push_index(&d->base) = idx1;
    }
  }

  if (d->base.track_count == 0) {
    ERROR_LOG("File '%s' contains no tracks", filename);
    Error_set_string_fmt(error, "File '%s' contains no tracks", filename);
    return false;
  }

  /* Lead-out. */
  toc_ent.cdte_track = 0xAA;
  if (ioctl(d->fd, CDROMREADTOCENTRY, &toc_ent) < 0) {
    Error_set_errno_prefix(error, "ioctl(CDROMREADTOCENTRY) for lead-out failed: ", errno);
    return false;
  }
  if (toc_ent.cdte_addr.lba < last_track_lba) {
    Error_set_string_fmt(error, "Lead-out LBA %d is less than last track %d",
                         toc_ent.cdte_addr.lba, last_track_lba);
    return false;
  }
  {
    const cd_image_lba_t prev_len = (cd_image_lba_t)(toc_ent.cdte_addr.lba - last_track_lba);
    cd_image_track_t* prev_track = &d->base.tracks[d->base.track_count - 1];
    cd_image_index_t* prev_index = &d->base.indices[d->base.index_count - 1];
    prev_track->length += prev_len;
    prev_index->length += prev_len;
    disc_lba += prev_len;
  }

  cd_image_add_lead_out_index(&d->base);
  d->base.lba_count = disc_lba;

  if (!determine_read_mode(d, error)) {
    Error_set_string(error, "Failed to determine a working read mode for device");
    return false;
  }

  return cd_image_seek_track_msf(&d->base, 1u, (cd_image_position_t){ 0, 0, 0 });
}

cd_image_t* cd_image_open_device(const char* path, Error* error)
{
  cd_image_device_t* d = alloc_device_image();
  if (!open_and_parse_device(d, path, error)) {
    cd_image_destroy(&d->base);
    return NULL;
  }
  return &d->base;
}

bool cd_image_is_device_name(const char* path)
{
  if (strncmp(path, "/dev/", 5) != 0) return false;
  const int fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) return false;
  const bool is_cdrom = (ioctl(fd, CDROM_GET_CAPABILITY, 0) >= 0);
  close(fd);
  return is_cdrom;
}

cd_image_device_list_t cd_image_get_device_list(void)
{
  cd_image_device_list_t out = { NULL, NULL, 0 };
  size_t cap = 0;

  struct udev* udev_ctx = udev_new();
  if (!udev_ctx) return out;

  struct udev_enumerate* en = udev_enumerate_new(udev_ctx);
  if (en) {
    udev_enumerate_add_match_subsystem(en, "block");
    udev_enumerate_add_match_property(en, "ID_CDROM", "1");
    udev_enumerate_scan_devices(en);
    struct udev_list_entry* devices = udev_enumerate_get_list_entry(en);
    struct udev_list_entry* it;
    udev_list_entry_foreach(it, devices) {
      const char* sysp = udev_list_entry_get_name(it);
      struct udev_device* dev = udev_device_new_from_syspath(udev_ctx, sysp);
      if (dev) {
        const char* devnode = udev_device_get_devnode(dev);
        if (devnode) {
          if (out.count == cap) {
            cap = cap ? cap * 2 : 4;
            out.paths = (char**)realloc(out.paths, cap * sizeof(char*));
            out.names = (char**)realloc(out.names, cap * sizeof(char*));
            if (!out.paths || !out.names) abort();
          }
          out.paths[out.count] = strdup(devnode);
          out.names[out.count] = strdup(devnode);
          if (!out.paths[out.count] || !out.names[out.count]) abort();
          out.count++;
        }
        udev_device_unref(dev);
      }
    }
    udev_enumerate_unref(en);
  }
  udev_unref(udev_ctx);
  return out;
}

void cd_image_device_list_destroy(cd_image_device_list_t* list)
{
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    free(list->paths[i]);
    free(list->names[i]);
  }
  free(list->paths);
  free(list->names);
  list->paths = NULL;
  list->names = NULL;
  list->count = 0;
}
