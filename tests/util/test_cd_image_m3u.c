#include "tests/test_harness.h"

#include "common/assert.h"
#include "common/error.h"
#include "util/cd_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define M3U_RAW_SECTOR_SIZE 2352

static void write_file(const char* path, const void* data, size_t len)
{
  FILE* fp = fopen(path, "wb");
  EXPECT_NOT_NULL(fp);
  if (len) {
    EXPECT_EQ(fwrite(data, 1, len, fp), len);
  }
  fclose(fp);
}

/* Writes a minimal MODE2/2352 single-sector .bin and a matching .cue.  Both
 * paths are returned via heap-allocated *out_*; caller frees and unlinks. */
static void make_disc(const char* dir, const char* stem,
                      char** out_cue, char** out_bin)
{
  char* cue_path = NULL;
  char* bin_path = NULL;
  if (asprintf(&cue_path, "%s/%s.cue", dir, stem) < 0) Panic("asprintf");
  if (asprintf(&bin_path, "%s/%s.bin", dir, stem) < 0) Panic("asprintf");

  unsigned char sector[M3U_RAW_SECTOR_SIZE];
  memset(sector, 0, sizeof(sector));
  write_file(bin_path, sector, sizeof(sector));

  char cue_buf[512];
  const int cue_len = snprintf(cue_buf, sizeof(cue_buf),
    "FILE \"%s.bin\" BINARY\n"
    "  TRACK 01 MODE2/2352\n"
    "    INDEX 01 00:00:00\n",
    stem);
  EXPECT_TRUE(cue_len > 0 && cue_len < (int)sizeof(cue_buf));
  write_file(cue_path, cue_buf, (size_t)cue_len);

  *out_cue = cue_path;
  *out_bin = bin_path;
}

static char* make_tmpdir(void)
{
  char tmpl[] = "/tmp/cupid_ps1_m3u_XXXXXX";
  if (!mkdtemp(tmpl)) Panic("mkdtemp");
  return strdup(tmpl);
}

static void unlink_quiet(const char* p) { if (p) unlink(p); }
static void rmdir_quiet (const char* p) { if (p) rmdir(p); }

TEST(CDImageM3u, OpensAndReportsTwoEntries)
{
  char* dir   = make_tmpdir();
  char* cueA  = NULL; char* binA = NULL;
  char* cueB  = NULL; char* binB = NULL;
  make_disc(dir, "discA", &cueA, &binA);
  make_disc(dir, "discB", &cueB, &binB);

  char* m3u_path = NULL;
  if (asprintf(&m3u_path, "%s/playlist.m3u", dir) < 0) Panic("asprintf");
  /* Use relative paths inside the m3u; cd_image_m3u resolves against the m3u dir. */
  const char* m3u_body =
    "# leading comment\n"
    "\n"
    "discA.cue\n"
    "  discB.cue   \n"     /* whitespace trim test */
    "# trailing comment\n";
  write_file(m3u_path, m3u_body, strlen(m3u_body));

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(m3u_path, &err);
  EXPECT_NOT_NULL(img);
  if (img) {
    EXPECT_TRUE(cd_image_has_sub_images(img));
    EXPECT_EQ(cd_image_get_sub_image_count(img), 2u);
    EXPECT_EQ(cd_image_get_current_sub_image(img), 0u);

    char* tA = cd_image_get_sub_image_title(img, 0);
    char* tB = cd_image_get_sub_image_title(img, 1);
    EXPECT_NOT_NULL(tA);
    EXPECT_NOT_NULL(tB);
    if (tA) EXPECT_STREQ(tA, "discA");
    if (tB) EXPECT_STREQ(tB, "discB");
    free(tA); free(tB);

    /* Out-of-range title returns NULL. */
    EXPECT_NULL(cd_image_get_sub_image_title(img, 99));

    /* Switch to second disc. */
    EXPECT_TRUE(cd_image_switch_sub_image(img, 1, &err));
    EXPECT_EQ(cd_image_get_current_sub_image(img), 1u);

    /* Same index is a no-op (still true). */
    EXPECT_TRUE(cd_image_switch_sub_image(img, 1, &err));

    /* OOB index fails; index unchanged. */
    Error_destroy(&err); Error_init(&err);
    EXPECT_FALSE(cd_image_switch_sub_image(img, 99, &err));
    EXPECT_EQ(cd_image_get_current_sub_image(img), 1u);

    cd_image_destroy(img);
  }
  Error_destroy(&err);

  unlink_quiet(m3u_path); free(m3u_path);
  unlink_quiet(cueA); unlink_quiet(binA); free(cueA); free(binA);
  unlink_quiet(cueB); unlink_quiet(binB); free(cueB); free(binB);
  rmdir_quiet(dir); free(dir);
}

TEST(CDImageM3u, EmptyPlaylistRejected)
{
  char* dir = make_tmpdir();
  char* m3u_path = NULL;
  if (asprintf(&m3u_path, "%s/empty.m3u", dir) < 0) Panic("asprintf");
  const char* body = "# only comments\n#and more\n\n  \n";
  write_file(m3u_path, body, strlen(body));

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(m3u_path, &err);
  EXPECT_NULL(img);
  Error_destroy(&err);

  unlink_quiet(m3u_path); free(m3u_path);
  rmdir_quiet(dir); free(dir);
}

TEST(CDImageM3u, MissingFileRejected)
{
  char* dir = make_tmpdir();
  char* m3u_path = NULL;
  if (asprintf(&m3u_path, "%s/missing.m3u", dir) < 0) Panic("asprintf");
  const char* body = "no_such_disc.cue\n";
  write_file(m3u_path, body, strlen(body));

  Error err; Error_init(&err);
  cd_image_t* img = cd_image_open(m3u_path, &err);
  EXPECT_NULL(img);
  Error_destroy(&err);

  unlink_quiet(m3u_path); free(m3u_path);
  rmdir_quiet(dir); free(dir);
}
