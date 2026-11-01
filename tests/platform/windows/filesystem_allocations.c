/* cupid-ps1 filesystem checks with nonzero bytes beyond allocation bounds. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void* filesystem_test_calloc(size_t count, size_t size) {
  if (size && count > (SIZE_MAX - 32) / size) return NULL;
  const size_t   bytes  = count * size;
  unsigned char* result = calloc(1, bytes + 32);
  /* Valid bytes retain calloc semantics. A native API that reads past the
   * supplied filename sees a suffix instead of an incidental heap terminator. */
  if (result) memset(result + bytes, 0x58, 16);
  return result;
}

#define calloc filesystem_test_calloc
#include "../../../src/common/windows/file_system.c"
