/* Smoke tests for the ECM (.ecm) open path.  The on-wire format encodes
 * (type, count) headers and reconstructs ECC/EDC on read; here we only
 * exercise the magic check + a minimal one-sector "raw" image, since a
 * real ECC/EDC fixture would require pulling in a full Mode2 sector. */
#include "tests/test_harness.h"

#include "common/assert.h"
#include "common/error.h"
#include "util/cd_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void write_file_bytes(const char* path, const void* data, size_t len)
{
  FILE* fp = fopen(path, "wb");
  EXPECT_NOT_NULL(fp);
  if (len) EXPECT_EQ(fwrite(data, 1, len, fp), len);
  fclose(fp);
}

static char* make_tmpdir(void)
{
  char tmpl[] = "/tmp/cupid_ps1_ecm_XXXXXX";
  if (!mkdtemp(tmpl)) Panic("mkdtemp");
  return strdup(tmpl);
}

static char* make_path(const char* dir, const char* name)
{
  char* out = NULL;
  if (asprintf(&out, "%s/%s", dir, name) < 0) Panic("asprintf");
  return out;
}

/* Reject when magic isn't "ECM\0". */
TEST(CDImageEcm, RejectsBadMagic)
{
  char* dir   = make_tmpdir();
  char* path  = make_path(dir, "bad.ecm");

  /* Wrong magic. */
  const unsigned char bad[8] = { 'X','Y','Z','Q', 0,0,0,0 };
  write_file_bytes(path, bad, sizeof(bad));

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(path, &err);
  EXPECT_NULL(img);
  Error_destroy(&err);

  unlink(path); free(path);
  rmdir(dir);   free(dir);
}

/* Reject if file is shorter than the 4-byte magic. */
TEST(CDImageEcm, RejectsTinyFile)
{
  char* dir   = make_tmpdir();
  char* path  = make_path(dir, "tiny.ecm");

  const unsigned char tiny[2] = { 'E','C' };
  write_file_bytes(path, tiny, sizeof(tiny));

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(path, &err);
  EXPECT_NULL(img);
  Error_destroy(&err);

  unlink(path); free(path);
  rmdir(dir);   free(dir);
}

/* Accept a single one-raw-sector image: magic + raw block (type=0,
 * count=2351 zero-based → 2352 bytes) + 2352 zero data bytes +
 * terminator (variable-length 0xFFFFFFFF: 5 bytes 0xFF FF FF FF 1F).
 *
 * Variable-length encoding of count:
 *   bit 7 = continuation flag
 *   first byte = (type & 3) | ((count & 0x1F) << 2) | (continue ? 0x80 : 0)
 *   continuation bytes carry (count >> 5+7k) & 0x7F | (cont ? 0x80 : 0)
 *
 * For raw count=2351 (0x92F = 0b1001_0010_1111):
 *   bits 0..4 = 0x0F  -> first byte = (0<<0)|(0x0F<<2)|0x80 = 0xBC
 *   bits 5..11 = 0x49 (no more bits) -> second byte = 0x49 (no cont)
 * For terminator count = 0xFFFFFFFF:
 *   bits 0..4 = 0x1F -> first = 0x00 | 0x7C | 0x80 = 0xFC
 *   bits 5..11 = 0x7F | 0x80 -> 0xFF
 *   bits 12..18 = 0x7F | 0x80 -> 0xFF
 *   bits 19..25 = 0x7F | 0x80 -> 0xFF
 *   bits 26..31 = 0x3F (no cont) -> 0x3F
 */
TEST(CDImageEcm, OpensMinimalRawImage)
{
  char* dir  = make_tmpdir();
  char* path = make_path(dir, "ok.ecm");

  unsigned char* buf = (unsigned char*)calloc(1, 4 + 2 + 2352 + 5);
  size_t off = 0;
  buf[off++] = 'E'; buf[off++] = 'C'; buf[off++] = 'M'; buf[off++] = 0x00;
  /* raw count=2351 (encoded zero-based, +1 = 2352 bytes) */
  buf[off++] = 0xBC;
  buf[off++] = 0x49;
  /* zeroed payload */
  off += 2352;
  /* terminator */
  buf[off++] = 0xFC;
  buf[off++] = 0xFF;
  buf[off++] = 0xFF;
  buf[off++] = 0xFF;
  buf[off++] = 0x3F;
  write_file_bytes(path, buf, off);
  free(buf);

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(path, &err);
  /* We reuse cd_image_open's extension dispatch which routes .ecm to
   * cd_image_open_ecm.  Single 2352-byte raw "sector" + 2-second pregap. */
  EXPECT_NOT_NULL(img);
  if (img) {
    EXPECT_EQ(cd_image_get_lba_count(img), 1u);
    EXPECT_EQ(cd_image_get_track_count(img), 1u);
    cd_image_destroy(img);
  }
  Error_destroy(&err);

  unlink(path); free(path);
  rmdir(dir);   free(dir);
}
