#include "entry.h"
#if !defined(_WIN32)
int main(int argc, char** argv) {
  return frontend_run(argc, argv);
}
#endif
