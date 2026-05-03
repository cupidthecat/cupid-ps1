/*
 *

 * LayeredSettingsInterface stores up to NUM_LAYERS settings_interface_t* and
 * writes through a layered interface; the caller is expected to write to a
 * specific layer directly.
 */
#ifndef CUPID_COMMON_LAYERED_SETTINGS_INTERFACE_H
#define CUPID_COMMON_LAYERED_SETTINGS_INTERFACE_H

#include "settings_interface.h"
#include "types.h"

typedef enum {
  LAYERED_SETTINGS_LAYER_GAME  = 0,
  LAYERED_SETTINGS_LAYER_INPUT = 1,
  LAYERED_SETTINGS_LAYER_BASE  = 2,
  LAYERED_SETTINGS_NUM_LAYERS  = 3,
} layered_settings_layer_t;

typedef struct {
  /* Must be first so a layered_settings_interface_t* converts to a
   * settings_interface_t*. */
  settings_interface_t base;
  settings_interface_t* layers[LAYERED_SETTINGS_NUM_LAYERS];
} layered_settings_interface_t;

void layered_settings_interface_init   (layered_settings_interface_t* self);
void layered_settings_interface_destroy(layered_settings_interface_t* self);

settings_interface_t* layered_settings_interface_get_layer(const layered_settings_interface_t* self,
                                                           layered_settings_layer_t layer);
void layered_settings_interface_set_layer(layered_settings_interface_t* self,
                                          layered_settings_layer_t layer,
                                          settings_interface_t* sif);

#endif /* CUPID_COMMON_LAYERED_SETTINGS_INTERFACE_H */
