/* cupid-ps1 TOC disc-type metadata and synthesized track classification. */
#include "tests/test_harness.h"
#include "common/error.h"
#include "util/disc/cd_image.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

TEST(CDImageDiscType, track_modes_copy_and_clear) {
  cd_image_t source, copy;
  cd_image_init(&source, NULL);
  cd_image_init(&copy, NULL);
  cd_image_track_t* track = cd_image_push_track(&source);
  for (u32 mode = CD_IMAGE_TRACK_MODE_AUDIO; mode <= CD_IMAGE_TRACK_MODE_MODE2_RAW; mode++) {
    track->mode = (cd_image_track_mode_t)mode;
    EXPECT_EQ(cd_image_get_disc_type(&source), mode >= CD_IMAGE_TRACK_MODE_MODE2 ? 0x20 : 0);
  }
  track->mode              = CD_IMAGE_TRACK_MODE_AUDIO;
  cd_image_track_t* second = cd_image_push_track(&source);
  second->mode             = CD_IMAGE_TRACK_MODE_MODE1;
  EXPECT_EQ(cd_image_get_disc_type(&source), 0);
  second->mode = CD_IMAGE_TRACK_MODE_MODE2_FORM1;
  EXPECT_EQ(cd_image_get_disc_type(&source), 0x20);
  source.has_toc_disc_type = true;
  source.toc_disc_type     = 0x10;
  EXPECT_EQ(cd_image_get_disc_type(&source), 0x10);
  cd_image_copy_toc(&copy, &source);
  EXPECT_EQ(cd_image_get_disc_type(&copy), 0x10);
  cd_image_clear_toc(&copy);
  EXPECT_FALSE(copy.has_toc_disc_type);
  EXPECT_EQ(cd_image_get_disc_type(&copy), 0);
  source.has_toc_disc_type = false;
  cd_image_copy_toc(&copy, &source);
  EXPECT_FALSE(copy.has_toc_disc_type);
  EXPECT_EQ(cd_image_get_disc_type(&copy), 0x20);
  free(source.tracks);
  free(copy.tracks);
  EXPECT_EQ(cd_image_get_disc_type(NULL), 0);
}

static void write_zero_sectors(const char* path, u32 size) {
  FILE* file = fopen(path, "wb");
  EXPECT_NOT_NULL(file);
  u8 zero[CD_IMAGE_RAW_SECTOR_SIZE] = {0};
  while (size) {
    const u32 count = size < sizeof(zero) ? size : sizeof(zero);
    EXPECT_EQ(fwrite(zero, 1, count, file), count);
    size -= count;
  }
  EXPECT_EQ(fclose(file), 0);
}

TEST(CDImageDiscType, ccd_a0_playlist_and_memory) {
  char directory[] = "/tmp/cupid_ps1_toc_type_XXXXXX";
  EXPECT_NOT_NULL(mkdtemp(directory));
  char image_path[256], sub_path[256], descriptor[256], playlist[256];
  snprintf(image_path, sizeof(image_path), "%s/disc.img", directory);
  snprintf(sub_path, sizeof(sub_path), "%s/disc.sub", directory);
  snprintf(descriptor, sizeof(descriptor), "%s/disc.ccd", directory);
  snprintf(playlist, sizeof(playlist), "%s/discs.m3u", directory);
  write_zero_sectors(image_path, 32 * CD_IMAGE_RAW_SECTOR_SIZE);
  write_zero_sectors(sub_path, 32 * 96);
  Error error;
  Error_init(&error);
  const u32 types[] = {0, 0x10, 0x20};
  for (u32 i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    FILE* file = fopen(descriptor, "wb");
    EXPECT_NOT_NULL(file);
    fprintf(file,
            "[CloneCD]\nVersion=3\n[Disc]\nTocEntries=3\n"
            "[Entry 0]\nPoint=0xA0\nPSec=%u\n"
            "[Entry 1]\nPoint=1\nControl=4\nPLBA=0\n"
            "[Entry 2]\nPoint=0xA2\nPLBA=32\n"
            "[TRACK 1]\nMODE=2\nINDEX 1=0\n",
            types[i]);
    EXPECT_EQ(fclose(file), 0);
    cd_image_t* image = cd_image_open(descriptor, &error);
    EXPECT_NOT_NULL(image);
    EXPECT_TRUE(image->has_toc_disc_type);
    EXPECT_EQ(cd_image_get_disc_type(image), types[i]);
    cd_image_t* memory = cd_image_create_memory_image(image, progress_callback_null(), &error);
    EXPECT_NOT_NULL(memory);
    EXPECT_EQ(cd_image_get_disc_type(memory), types[i]);
    cd_image_destroy(memory);
    cd_image_destroy(image);
    file = fopen(playlist, "wb");
    EXPECT_NOT_NULL(file);
    fputs("disc.ccd\n", file);
    EXPECT_EQ(fclose(file), 0);
    image = cd_image_open(playlist, &error);
    EXPECT_NOT_NULL(image);
    EXPECT_EQ(cd_image_get_disc_type(image), types[i]);
    cd_image_destroy(image);
  }
  Error_destroy(&error);
  unlink(playlist);
  unlink(descriptor);
  unlink(sub_path);
  unlink(image_path);
  rmdir(directory);
}
