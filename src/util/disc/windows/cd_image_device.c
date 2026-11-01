/* cupid-ps1 Windows disc-device boundary. Image files use the shared readers. */
#include "util/disc/cd_image.h"
#if defined(_WIN32)
#include "common/error.h"
#include <stdlib.h>
#include <string.h>

cd_image_t* cd_image_open_device(const char* path, Error* error) {
  (void)path;
  Error_set_string(error, "Physical CD-ROM access is not implemented on Windows; open a disc image.");
  return NULL;
}
bool cd_image_is_device_name(const char* path) {
  return path && (!strncmp(path, "\\\\.\\", 4) || !strncmp(path, "//./", 4));
}
cd_image_device_list_t cd_image_get_device_list(void) {
  return (cd_image_device_list_t){0};
}
void cd_image_device_list_destroy(cd_image_device_list_t* list) {
  if (!list) return;
  for (size_t i = 0; i < list->count; i++) {
    free(list->paths[i]);
    free(list->names[i]);
  }
  free(list->paths);
  free(list->names);
  *list = (cd_image_device_list_t){0};
}
#endif
