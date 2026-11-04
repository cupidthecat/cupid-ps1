/* cupid-ps1 Windows lexical path roots and separators. */
#include "common/path.h"
#include "common/align.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                              \
  do {                                                        \
    if (!(x)) {                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
      exit(1);                                                \
    }                                                         \
  } while (0)

int main(void) {
  small_string_stack_t path;
  small_string_stack_init(&path);
  const struct {
    const char* input;
    const char* expected;
  } cases[] = {
      {"C:\\a\\.\\b\\..\\file.bin", "C:/a/file.bin"},
      {"C:/../../a", "C:/a"},
      {"C:/a/..", "C:/"},
      {"C:../a/../../x", "C:../../x"},
      {"\\\\server\\share\\a\\..\\..\\x", "//server/share/x"},
      {"//server/share/../../", "//server/share/"},
      {"\\\\?\\C:\\a\\..", "//?/C:/"},
      {"\\\\?\\UNC\\server\\share\\..\\file", "//?/UNC/server/share/file"},
      {"\\\\?\\unc\\server\\share\\..\\file", "//?/unc/server/share/file"},
      {"//?/Volume{generated}/a/../..", "//?/Volume{generated}/"},
      {"../a/../../b", "../../b"},
      {".", "."},
      {"", ""},
  };
  for (size_t i = 0; i < countof(cases); i++) {
    path_canonicalize_view(&path.s, cases[i].input, (u32)strlen(cases[i].input));
    if (strcmp(small_string_c_str(&path.s), cases[i].expected)) {
      fprintf(stderr, "canonical '%s': '%s' != '%s'\n", cases[i].input, small_string_c_str(&path.s), cases[i].expected);
      return 1;
    }
  }
  CHECK(path_is_absolute_cstr("C:\\a") && path_is_absolute_cstr("//server/share"));
  CHECK(!path_is_absolute_cstr("C:a") && !path_is_absolute_cstr("relative/a"));
  path_to_native_cstr(&path.s, "\\\\server\\share\\a\\\\b\\");
  CHECK(!strcmp(small_string_c_str(&path.s), "//server/share/a/b"));
  path_combine_cstr2(&path.s, "C:/", "");
  CHECK(!strcmp(small_string_c_str(&path.s), "C:/"));
  path_combine_cstr2(&path.s, "\\\\server\\share\\", "file");
  CHECK(!strcmp(small_string_c_str(&path.s), "//server/share/file"));
  path_combine_cstr2(&path.s, "C:", "file");
  CHECK(!strcmp(small_string_c_str(&path.s), "C:file"));
  path_build_relative_cstr(&path.s, "C:\\media\\disc.cue", "track.bin");
  CHECK(!strcmp(small_string_c_str(&path.s), "C:\\media\\track.bin"));
  path_build_relative_cstr(&path.s, "C:/media/disc.cue", "D:\\tracks\\track.bin");
  CHECK(!strcmp(small_string_c_str(&path.s), "D:/tracks/track.bin"));
  path_make_relative_view(&path.s, "C:/A/dir/file", 13, "c:/a/other", 10);
  CHECK(!strcmp(small_string_c_str(&path.s), "../dir/file"));
  path_make_relative_view(&path.s, "D:/file", 7, "C:/base", 7);
  CHECK(!strcmp(small_string_c_str(&path.s), "D:/file"));
  const char* view;
  u32         len;
  path_get_file_name_cstr("C:\\media\\disc.cue", &view, &len);
  CHECK(len == 8 && !memcmp(view, "disc.cue", len));
  path_get_directory_cstr("C:\\disc.cue", &view, &len);
  CHECK(len == 3 && !memcmp(view, "C:\\", len));
  small_string_destroy(&path.s);
  for (size_t alignment = 16; alignment <= 4096; alignment *= 2) {
    void* buffer = AlignedMalloc(31, alignment);
    CHECK(buffer && !((uintptr_t)buffer & (alignment - 1)));
    memset(buffer, 0xAB, 31);
    AlignedFree(buffer);
  }
  CHECK(!AlignedMalloc(32, 3));
  AlignedFree(NULL);
  puts("Windows path roots, separators and aligned allocation passed");
  return 0;
}
