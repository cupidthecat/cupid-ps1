/* cupid-ps1 native frontend input notifications. */
#include "util/input/input_manager.h"
#include "common/log.h"
LOG_CHANNEL(Host);

void host_on_input_device_connected(input_binding_key_t key, const char* identifier, const char* name) {
  (void)key;
  INFO_LOG("Device connected: %s (%s)", identifier ? identifier : "?", name ? name : "?");
}
void host_on_input_device_disconnected(input_binding_key_t key, const char* identifier) {
  (void)key;
  INFO_LOG("Device disconnected: %s", identifier ? identifier : "?");
}
