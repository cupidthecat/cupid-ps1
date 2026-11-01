/* cupid-ps1 Windows Unicode command-line entry. */
#include "frontend/entry.h"
#include "common/windows/windows_util.h"
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t** wide_argv);
int wmain(int argc, wchar_t** wide_argv) {
  char** argv = calloc((size_t)argc + 1, sizeof(*argv));
  if (!argv) return EXIT_FAILURE;
  int result = EXIT_FAILURE;
  for (int i = 0; i < argc; i++) {
    argv[i] = windows_wide_to_utf8(wide_argv[i], NULL);
    if (!argv[i]) {
      fputs("Invalid Unicode command line or insufficient memory\n", stderr);
      goto done;
    }
  }
  result = frontend_run(argc, argv);
done:
  for (int i = 0; i < argc; i++)
    free(argv[i]);
  free(argv);
  return result;
}
