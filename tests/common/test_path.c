 /*
 * small_string-output API. Linux-only (FS_OSPATH_SEPARATOR_CHARACTER == '/'). */

#include "tests/test_harness.h"

#include "common/path.h"
#include "common/small_string.h"

#include <string.h>

static void to_native(small_string_t* out, const char* path)
{
  small_string_init(out);
  path_to_native_cstr(out, path);
}

static void combine2(small_string_t* out, const char* a, const char* b)
{
  small_string_init(out);
  path_combine_cstr2(out, a, b);
}

#define EXPECT_PATH_EQ(actual_ss, expected_cstr)                                   \
  do {                                                                              \
    small_string_t _exp;                                                            \
    to_native(&_exp, (expected_cstr));                                              \
    EXPECT_STREQ(small_string_c_str(&(actual_ss)), small_string_c_str(&_exp));      \
    small_string_destroy(&_exp);                                                    \
  } while (0)

TEST(Path, IsAbsolute)
{
  EXPECT_FALSE(path_is_absolute_cstr(""));
  EXPECT_FALSE(path_is_absolute_cstr("foo"));
  EXPECT_FALSE(path_is_absolute_cstr("foo/bar"));
  EXPECT_TRUE (path_is_absolute_cstr("/foo/bar"));
  EXPECT_TRUE (path_is_absolute_cstr("/"));
  EXPECT_TRUE (path_is_absolute_cstr("/path"));
}

TEST(Path, ToNativePath)
{
  small_string_t out;
  to_native(&out, "");
  EXPECT_STREQ(small_string_c_str(&out), "");
  small_string_destroy(&out);

  to_native(&out, "foo");
  EXPECT_STREQ(small_string_c_str(&out), "foo");
  small_string_destroy(&out);

  to_native(&out, "foo/");
  EXPECT_STREQ(small_string_c_str(&out), "foo");
  small_string_destroy(&out);

  to_native(&out, "foo//bar");
  EXPECT_STREQ(small_string_c_str(&out), "foo/bar");
  small_string_destroy(&out);

  to_native(&out, "/foo/bar/baz");
  EXPECT_STREQ(small_string_c_str(&out), "/foo/bar/baz");
  small_string_destroy(&out);
}

TEST(Path, Combine)
{
  small_string_t out;

  combine2(&out, "foo", "bar");
  EXPECT_PATH_EQ(out, "foo/bar");
  small_string_destroy(&out);

  combine2(&out, "foo/bar", "baz");
  EXPECT_PATH_EQ(out, "foo/bar/baz");
  small_string_destroy(&out);

  combine2(&out, "foo/bar/", "/baz/");
  EXPECT_PATH_EQ(out, "foo/bar/baz");
  small_string_destroy(&out);

  combine2(&out, "/foo/bar", "baz");
  EXPECT_PATH_EQ(out, "/foo/bar/baz");
  small_string_destroy(&out);
}

TEST(Path, Canonicalize)
{
  small_string_t out;
  small_string_init(&out);
  path_canonicalize_view(&out, "foo/bar/../baz", (u32)strlen("foo/bar/../baz"));
  EXPECT_PATH_EQ(out, "foo/baz");
  small_string_destroy(&out);

  small_string_init(&out);
  path_canonicalize_view(&out, "foo/./bar/./baz", (u32)strlen("foo/./bar/./baz"));
  EXPECT_PATH_EQ(out, "foo/bar/baz");
  small_string_destroy(&out);

  small_string_init(&out);
  path_canonicalize_view(&out, "foo/bar/../baz/../foo", (u32)strlen("foo/bar/../baz/../foo"));
  EXPECT_PATH_EQ(out, "foo/foo");
  small_string_destroy(&out);

  small_string_init(&out);
  path_canonicalize_view(&out, "./foo", 5);
  EXPECT_PATH_EQ(out, "foo");
  small_string_destroy(&out);
}

TEST(Path, GetExtension)
{
  const char* ext_data; u32 ext_len;
  path_get_extension_cstr("foo.txt", &ext_data, &ext_len);
  EXPECT_EQ(ext_len, 3u);
  EXPECT_TRUE(memcmp(ext_data, "txt", 3) == 0);

  path_get_extension_cstr("foo.tar.gz", &ext_data, &ext_len);
  EXPECT_EQ(ext_len, 2u);
  EXPECT_TRUE(memcmp(ext_data, "gz", 2) == 0);

  path_get_extension_cstr("noext", &ext_data, &ext_len);
  EXPECT_EQ(ext_len, 0u);
}

TEST(Path, GetFileName)
{
  const char* data; u32 len;
  path_get_file_name_cstr("/foo/bar/baz.txt", &data, &len);
  EXPECT_EQ(len, 7u);
  EXPECT_TRUE(memcmp(data, "baz.txt", 7) == 0);

  path_get_file_name_cstr("baz.txt", &data, &len);
  EXPECT_EQ(len, 7u);
  EXPECT_TRUE(memcmp(data, "baz.txt", 7) == 0);

  path_get_file_name_cstr("/", &data, &len);
  EXPECT_EQ(len, 0u);
}
