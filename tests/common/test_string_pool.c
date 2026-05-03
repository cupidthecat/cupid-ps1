#include "tests/test_harness.h"

#include "common/string_pool.h"

#include <string.h>

#define LIT_LEN(s) ((size_t)(sizeof(s) - 1))

/* eval actual_ptr first (which writes through &n via the get() side effect),
 * THEN read actual_len.  C11 sequences each declarator's init before the next. */
#define EXPECT_STRVIEW(actual_ptr, actual_len, lit) \
  do {                                                                      \
    const char* _p = (actual_ptr);                                          \
    size_t _n = (size_t)(actual_len);                                       \
    EXPECT_EQ(_n, LIT_LEN(lit));                                            \
    EXPECT_TRUE(_p != NULL && memcmp(_p, (lit), LIT_LEN(lit)) == 0);        \
  } while (0)

TEST(BumpStringPool, InitiallyEmpty)
{
  bump_string_pool_t pool;
  bump_string_pool_init(&pool);
  EXPECT_TRUE(bump_string_pool_empty(&pool));
  EXPECT_EQ(bump_string_pool_size(&pool), 0u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, AddSingleString)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t off = bump_string_pool_add(&pool, "hello", 5);
  EXPECT_NE(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_FALSE(bump_string_pool_empty(&pool));
  size_t n; const char* s = bump_string_pool_get(&pool, off, &n);
  EXPECT_STRVIEW(s, n, "hello");
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, AddEmptyStringReturnsInvalid)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t off = bump_string_pool_add(&pool, "", 0);
  EXPECT_EQ(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_TRUE(bump_string_pool_empty(&pool));
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, AddMultipleStrings)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t o1 = bump_string_pool_add(&pool, "alpha", 5);
  size_t o2 = bump_string_pool_add(&pool, "beta", 4);
  size_t o3 = bump_string_pool_add(&pool, "gamma", 5);
  EXPECT_NE(o1, o2);
  EXPECT_NE(o2, o3);
  size_t n;
  EXPECT_STRVIEW(bump_string_pool_get(&pool, o1, &n), n, "alpha");
  EXPECT_STRVIEW(bump_string_pool_get(&pool, o2, &n), n, "beta");
  EXPECT_STRVIEW(bump_string_pool_get(&pool, o3, &n), n, "gamma");
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, DuplicateStringsStored)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t o1 = bump_string_pool_add(&pool, "same", 4);
  size_t s1 = bump_string_pool_size(&pool);
  size_t o2 = bump_string_pool_add(&pool, "same", 4);
  EXPECT_NE(o1, o2);
  EXPECT_TRUE(bump_string_pool_size(&pool) > s1);
  size_t n;
  EXPECT_STRVIEW(bump_string_pool_get(&pool, o1, &n), n, "same");
  EXPECT_STRVIEW(bump_string_pool_get(&pool, o2, &n), n, "same");
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, GetStringWithOffsetAndLength)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t off = bump_string_pool_add(&pool, "hello world", 11);
  const char* a = bump_string_pool_get_n(&pool, off, 5);
  EXPECT_TRUE(a != NULL && memcmp(a, "hello", 5) == 0);
  const char* b = bump_string_pool_get_n(&pool, off, 11);
  EXPECT_TRUE(b != NULL && memcmp(b, "hello world", 11) == 0);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, GetStringInvalidOffset)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t n;
  EXPECT_NULL(bump_string_pool_get(&pool, STRING_POOL_INVALID_OFFSET, &n));
  EXPECT_EQ(n, 0u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, GetStringOutOfBounds)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  (void)bump_string_pool_add(&pool, "test", 4);
  size_t n;
  EXPECT_NULL(bump_string_pool_get(&pool, 9999, &n));
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, GetStringWithLengthOutOfBounds)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t off = bump_string_pool_add(&pool, "short", 5);
  EXPECT_NULL(bump_string_pool_get_n(&pool, off, 9999));
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, SizeIncludesNullTerminators)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  (void)bump_string_pool_add(&pool, "abc", 3);
  EXPECT_EQ(bump_string_pool_size(&pool), 4u);
  (void)bump_string_pool_add(&pool, "de", 2);
  EXPECT_EQ(bump_string_pool_size(&pool), 7u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, Clear)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  (void)bump_string_pool_add(&pool, "one", 3);
  (void)bump_string_pool_add(&pool, "two", 3);
  EXPECT_FALSE(bump_string_pool_empty(&pool));
  bump_string_pool_clear(&pool);
  EXPECT_TRUE(bump_string_pool_empty(&pool));
  EXPECT_EQ(bump_string_pool_size(&pool), 0u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, ClearThenReuse)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  (void)bump_string_pool_add(&pool, "before", 6);
  bump_string_pool_clear(&pool);
  size_t off = bump_string_pool_add(&pool, "after", 5);
  EXPECT_NE(off, STRING_POOL_INVALID_OFFSET);
  size_t n;
  EXPECT_STRVIEW(bump_string_pool_get(&pool, off, &n), n, "after");
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, ReserveDoesNotChangeState)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  bump_string_pool_reserve(&pool, 4096);
  EXPECT_TRUE(bump_string_pool_empty(&pool));
  EXPECT_EQ(bump_string_pool_size(&pool), 0u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, UnicodeStrings)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  const char jp[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88"; /* "日本語テスト" in UTF-8 */
  size_t jp_off = bump_string_pool_add(&pool, jp, sizeof(jp) - 1);
  size_t n;
  const char* g = bump_string_pool_get(&pool, jp_off, &n);
  EXPECT_EQ(n, sizeof(jp) - 1);
  EXPECT_TRUE(g != NULL && memcmp(g, jp, sizeof(jp) - 1) == 0);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, SingleCharacterString)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t off = bump_string_pool_add(&pool, "x", 1);
  size_t n;
  EXPECT_STRVIEW(bump_string_pool_get(&pool, off, &n), n, "x");
  EXPECT_EQ(bump_string_pool_size(&pool), 2u);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, VeryLongString)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  char* big = (char*)malloc(10000);
  memset(big, 'z', 10000);
  size_t off = bump_string_pool_add(&pool, big, 10000);
  EXPECT_EQ(bump_string_pool_size(&pool), 10001u);
  size_t n;
  const char* got = bump_string_pool_get(&pool, off, &n);
  EXPECT_EQ(n, 10000u);
  EXPECT_TRUE(got != NULL && memcmp(got, big, 10000) == 0);
  free(big);
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, ManyStrings)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  bump_string_pool_reserve(&pool, 2000);
  for (int i = 0; i < 200; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "str_%d", i);
    size_t off = bump_string_pool_add(&pool, buf, (size_t)len);
    size_t n;
    const char* g = bump_string_pool_get(&pool, off, &n);
    EXPECT_EQ((int)n, len);
    EXPECT_TRUE(g != NULL && memcmp(g, buf, (size_t)len) == 0);
  }
  bump_string_pool_destroy(&pool);
}

TEST(BumpStringPool, SpecialCharacters)
{
  bump_string_pool_t pool; bump_string_pool_init(&pool);
  size_t tab = bump_string_pool_add(&pool, "a\tb", 3);
  size_t nl  = bump_string_pool_add(&pool, "c\nd", 3);
  size_t n;
  EXPECT_STRVIEW(bump_string_pool_get(&pool, tab, &n), n, "a\tb");
  EXPECT_STRVIEW(bump_string_pool_get(&pool, nl,  &n), n, "c\nd");
  bump_string_pool_destroy(&pool);
}

TEST(BumpUniqueStringPool, InitiallyEmpty)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  EXPECT_TRUE(bump_unique_string_pool_empty(&p));
  EXPECT_EQ(bump_unique_string_pool_size(&p), 0u);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 0u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, AddSingleString)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t off = bump_unique_string_pool_add(&p, "hello", 5);
  EXPECT_NE(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 1u);
  size_t n;
  EXPECT_STRVIEW(bump_unique_string_pool_get(&p, off, &n), n, "hello");
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, AddEmptyReturnsInvalid)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t off = bump_unique_string_pool_add(&p, "", 0);
  EXPECT_EQ(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_TRUE(bump_unique_string_pool_empty(&p));
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, DeduplicatesSameString)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t o1 = bump_unique_string_pool_add(&p, "duplicate", 9);
  size_t s1 = bump_unique_string_pool_size(&p);
  size_t c1 = bump_unique_string_pool_count(&p);
  size_t o2 = bump_unique_string_pool_add(&p, "duplicate", 9);
  EXPECT_EQ(o1, o2);
  EXPECT_EQ(bump_unique_string_pool_size(&p), s1);
  EXPECT_EQ(bump_unique_string_pool_count(&p), c1);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, DeduplicateMultipleAdds)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t off = bump_unique_string_pool_add(&p, "test", 4);
  size_t s = bump_unique_string_pool_size(&p);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(bump_unique_string_pool_add(&p, "test", 4), off);
  }
  EXPECT_EQ(bump_unique_string_pool_size(&p), s);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 1u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, DistinguishesDifferentStrings)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t a = bump_unique_string_pool_add(&p, "alpha", 5);
  size_t b = bump_unique_string_pool_add(&p, "beta", 4);
  EXPECT_NE(a, b);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 2u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, ManyUniqueStrings)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  bump_unique_string_pool_reserve(&p, 200, 2000);
  for (int i = 0; i < 200; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "string_%d", i);
    (void)bump_unique_string_pool_add(&p, buf, (size_t)len);
  }
  EXPECT_EQ(bump_unique_string_pool_count(&p), 200u);
  for (int i = 0; i < 200; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "string_%d", i);
    size_t off = bump_unique_string_pool_add(&p, buf, (size_t)len);
    size_t n;
    const char* g = bump_unique_string_pool_get(&p, off, &n);
    EXPECT_EQ((int)n, len);
    EXPECT_TRUE(g != NULL && memcmp(g, buf, (size_t)len) == 0);
  }
  EXPECT_EQ(bump_unique_string_pool_count(&p), 200u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, CaseSensitive)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t a = bump_unique_string_pool_add(&p, "hello", 5);
  size_t b = bump_unique_string_pool_add(&p, "Hello", 5);
  size_t c = bump_unique_string_pool_add(&p, "HELLO", 5);
  EXPECT_NE(a, b);
  EXPECT_NE(b, c);
  EXPECT_NE(a, c);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 3u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, PrefixSuffixDistinct)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t a = bump_unique_string_pool_add(&p, "abc", 3);
  size_t b = bump_unique_string_pool_add(&p, "abcd", 4);
  size_t c = bump_unique_string_pool_add(&p, "ab", 2);
  EXPECT_NE(a, b); EXPECT_NE(a, c); EXPECT_NE(b, c);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 3u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, EmbeddedNull)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  const char with_null[3] = {'a', '\0', 'b'};
  size_t off = bump_unique_string_pool_add(&p, with_null, 3);
  const char* g = bump_unique_string_pool_get_n(&p, off, 3);
  EXPECT_TRUE(g != NULL && memcmp(g, with_null, 3) == 0);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, InterleaveAddAndLookup)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  size_t a = bump_unique_string_pool_add(&p, "zebra", 5);
  size_t b = bump_unique_string_pool_add(&p, "apple", 5);
  size_t c = bump_unique_string_pool_add(&p, "mango", 5);
  size_t n;
  EXPECT_STRVIEW(bump_unique_string_pool_get(&p, a, &n), n, "zebra");
  EXPECT_STRVIEW(bump_unique_string_pool_get(&p, b, &n), n, "apple");
  EXPECT_STRVIEW(bump_unique_string_pool_get(&p, c, &n), n, "mango");
  EXPECT_EQ(bump_unique_string_pool_add(&p, "mango", 5), c);
  EXPECT_EQ(bump_unique_string_pool_add(&p, "apple", 5), b);
  EXPECT_EQ(bump_unique_string_pool_add(&p, "zebra", 5), a);
  EXPECT_EQ(bump_unique_string_pool_count(&p), 3u);
  bump_unique_string_pool_destroy(&p);
}

TEST(BumpUniqueStringPool, Clear)
{
  bump_unique_string_pool_t p; bump_unique_string_pool_init(&p);
  (void)bump_unique_string_pool_add(&p, "one", 3);
  (void)bump_unique_string_pool_add(&p, "two", 3);
  bump_unique_string_pool_clear(&p);
  EXPECT_TRUE(bump_unique_string_pool_empty(&p));
  EXPECT_EQ(bump_unique_string_pool_count(&p), 0u);
  bump_unique_string_pool_destroy(&p);
}

TEST(StringPool, InitiallyEmpty)
{
  string_pool_t p; string_pool_init(&p);
  EXPECT_TRUE(string_pool_empty(&p));
  EXPECT_EQ(string_pool_size(&p), 0u);
  EXPECT_EQ(string_pool_count(&p), 0u);
  string_pool_destroy(&p);
}

TEST(StringPool, AddSingleString)
{
  string_pool_t p; string_pool_init(&p);
  size_t off = string_pool_add(&p, "hello", 5);
  EXPECT_NE(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_EQ(string_pool_count(&p), 1u);
  size_t n;
  EXPECT_STRVIEW(string_pool_get(&p, off, &n), n, "hello");
  string_pool_destroy(&p);
}

TEST(StringPool, AddEmptyReturnsInvalid)
{
  string_pool_t p; string_pool_init(&p);
  size_t off = string_pool_add(&p, "", 0);
  EXPECT_EQ(off, STRING_POOL_INVALID_OFFSET);
  EXPECT_TRUE(string_pool_empty(&p));
  string_pool_destroy(&p);
}

TEST(StringPool, Deduplicates)
{
  string_pool_t p; string_pool_init(&p);
  size_t o1 = string_pool_add(&p, "duplicate", 9);
  size_t s1 = string_pool_size(&p);
  size_t o2 = string_pool_add(&p, "duplicate", 9);
  EXPECT_EQ(o1, o2);
  EXPECT_EQ(string_pool_size(&p), s1);
  EXPECT_EQ(string_pool_count(&p), 1u);
  string_pool_destroy(&p);
}

TEST(StringPool, DeduplicateManyAdds)
{
  string_pool_t p; string_pool_init(&p);
  size_t off = string_pool_add(&p, "test", 4);
  size_t s = string_pool_size(&p);
  for (int i = 0; i < 100; i++) {
    EXPECT_EQ(string_pool_add(&p, "test", 4), off);
  }
  EXPECT_EQ(string_pool_size(&p), s);
  EXPECT_EQ(string_pool_count(&p), 1u);
  string_pool_destroy(&p);
}

TEST(StringPool, ManyUnique)
{
  string_pool_t p; string_pool_init(&p);
  string_pool_reserve(&p, 2000);
  for (int i = 0; i < 200; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "string_%d", i);
    (void)string_pool_add(&p, buf, (size_t)len);
  }
  EXPECT_EQ(string_pool_count(&p), 200u);
  for (int i = 0; i < 200; i++) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "string_%d", i);
    size_t off = string_pool_add(&p, buf, (size_t)len);
    size_t n;
    const char* g = string_pool_get(&p, off, &n);
    EXPECT_EQ((int)n, len);
    EXPECT_TRUE(g != NULL && memcmp(g, buf, (size_t)len) == 0);
  }
  EXPECT_EQ(string_pool_count(&p), 200u);
  string_pool_destroy(&p);
}

TEST(StringPool, SubstringNotDeduplicated)
{
  string_pool_t p; string_pool_init(&p);
  size_t full = string_pool_add(&p, "hello world", 11);
  size_t sub  = string_pool_add(&p, "hello", 5);
  EXPECT_NE(full, sub);
  EXPECT_EQ(string_pool_count(&p), 2u);
  string_pool_destroy(&p);
}

TEST(StringPool, GetStringInvalidOffset)
{
  string_pool_t p; string_pool_init(&p);
  size_t n;
  EXPECT_NULL(string_pool_get(&p, STRING_POOL_INVALID_OFFSET, &n));
  EXPECT_EQ(n, 0u);
  string_pool_destroy(&p);
}

TEST(StringPool, ReuseAfterClear)
{
  string_pool_t p; string_pool_init(&p);
  size_t o1 = string_pool_add(&p, "reuse", 5);
  EXPECT_EQ(o1, 0u);
  EXPECT_EQ(string_pool_count(&p), 1u);
  string_pool_clear(&p);
  size_t o2 = string_pool_add(&p, "reuse", 5);
  EXPECT_EQ(string_pool_count(&p), 1u);
  EXPECT_EQ(o2, 0u);
  size_t n;
  EXPECT_STRVIEW(string_pool_get(&p, o2, &n), n, "reuse");
  string_pool_destroy(&p);
}
