#include "layered_settings_interface.h"

#include "assert.h"
#include "settings_interface.h"
#include "small_string.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>

 /*
 * same FIRST_LAYER..LAST_LAYER range; everything below mirrors that. */

static bool layered_is_empty(settings_interface_t* self)
{
  (void)self;
  return false;
}

static bool layered_lookup_value(const settings_interface_t* self, const char* section,
                                 const char* key, small_string_t* out)
{
  const layered_settings_interface_t* l = (const layered_settings_interface_t*)self;
  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++) {
    settings_interface_t* sif = l->layers[i];
    if (sif && sif->vtbl->lookup_value(sif, section, key, out))
      return true;
  }
  return false;
}

static void layered_store_value(settings_interface_t* self, const char* section,
                                const char* key, const char* value)
{
  (void)self; (void)section; (void)key; (void)value;
  Panic("Attempt to call store_value() on layered settings interface");
}

static bool layered_contains_value(const settings_interface_t* self, const char* section,
                                   const char* key)
{
  const layered_settings_interface_t* l = (const layered_settings_interface_t*)self;
  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++) {
    settings_interface_t* sif = l->layers[i];
    if (sif && sif->vtbl->contains_value(sif, section, key))
      return true;
  }
  return false;
}

static void layered_delete_value(settings_interface_t* self, const char* section, const char* key)
{
  (void)self; (void)section; (void)key;
  Panic("Attempt to call delete_value() on layered settings interface");
}

static void layered_clear_section(settings_interface_t* self, const char* section)
{
  (void)self; (void)section;
  Panic("Attempt to call clear_section() on layered settings interface");
}

static void layered_remove_section(settings_interface_t* self, const char* section)
{
  (void)self; (void)section;
  Panic("Attempt to call remove_section() on layered settings interface");
}

static void layered_remove_empty_sections(settings_interface_t* self)
{
  (void)self;
  Panic("Attempt to call remove_empty_sections() on layered settings interface");
}

static void layered_get_string_list(const settings_interface_t* self, const char* section,
                                    const char* key, char*** out_array, size_t* out_count)
{
  const layered_settings_interface_t* l = (const layered_settings_interface_t*)self;
  *out_array = NULL;
  *out_count = 0;
  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++) {
    settings_interface_t* sif = l->layers[i];
    if (!sif)
      continue;
    char** arr = NULL;
    size_t count = 0;
    sif->vtbl->get_string_list(sif, section, key, &arr, &count);
    if (count > 0) {
      *out_array = arr;
      *out_count = count;
      return;
    }
    /* Empty result still needs freeing if the impl allocated an empty array. */
    settings_interface_free_string_list(arr, count);
  }
}

static void layered_set_string_list(settings_interface_t* self, const char* section,
                                    const char* key, const char* const* items, size_t count)
{
  (void)self; (void)section; (void)key; (void)items; (void)count;
  Panic("Attempt to call set_string_list() on layered settings interface");
}

static bool layered_remove_from_string_list(settings_interface_t* self, const char* section,
                                            const char* key, const char* item)
{
  (void)self; (void)section; (void)key; (void)item;
  Panic("Attempt to call remove_from_string_list() on layered settings interface");
}

static bool layered_add_to_string_list(settings_interface_t* self, const char* section,
                                       const char* key, const char* item)
{
  (void)self; (void)section; (void)key; (void)item;
  Panic("Attempt to call add_to_string_list() on layered settings interface");
}

 /*
 * O(1) lookup, but key counts per section in ini files are tiny (tens at most)
 * so the linear scan is comparable and avoids pulling in a hash set. */
static bool key_already_seen(char** keys, size_t count, const char* needle)
{
  for (size_t i = 0; i < count; i++) {
    if (strcmp(keys[i], needle) == 0)
      return true;
  }
  return false;
}

static void layered_get_key_value_list(const settings_interface_t* self, const char* section,
                                       char*** out_keys, char*** out_values, size_t* out_count)
{
  const layered_settings_interface_t* l = (const layered_settings_interface_t*)self;

  char** out_k = NULL;
  char** out_v = NULL;
  size_t out_n = 0;
  size_t cap   = 0;

  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++) {
    settings_interface_t* sif = l->layers[i];
    if (!sif)
      continue;

    char** layer_keys   = NULL;
    char** layer_values = NULL;
    size_t layer_count  = 0;
    sif->vtbl->get_key_value_list(sif, section, &layer_keys, &layer_values, &layer_count);

     /*
     * layer so that layers can return multiple entries for a single key. We
     * emulate that by recording the cutoff and only filtering against keys
     * that existed before this layer started. */
    const size_t pre_count = out_n;
    for (size_t j = 0; j < layer_count; j++) {
      if (key_already_seen(out_k, pre_count, layer_keys[j])) {
        free(layer_keys[j]);
        free(layer_values[j]);
        continue;
      }
      if (out_n == cap) {
        const size_t new_cap = cap ? cap * 2 : 8;
        out_k = realloc(out_k, new_cap * sizeof(char*));
        out_v = realloc(out_v, new_cap * sizeof(char*));
        cap = new_cap;
      }
      /* Take ownership of the duplicated strings instead of copying. */
      out_k[out_n] = layer_keys[j];
      out_v[out_n] = layer_values[j];
      out_n++;
    }
    /* Free only the array spines; element ownership transferred above. */
    free(layer_keys);
    free(layer_values);
  }

  *out_keys   = out_k;
  *out_values = out_v;
  *out_count  = out_n;
}

static void layered_set_key_value_list(settings_interface_t* self, const char* section,
                                       const char* const* keys, const char* const* values, size_t count)
{
  (void)self; (void)section; (void)keys; (void)values; (void)count;
  Panic("Attempt to call set_key_value_list() on layered settings interface");
}

static const settings_interface_vtable_t k_layered_vtable = {
   .is_empty                = layered_is_empty,
  .lookup_value            = layered_lookup_value,
  .store_value             = layered_store_value,
  .get_string_list         = layered_get_string_list,
  .set_string_list         = layered_set_string_list,
  .remove_from_string_list = layered_remove_from_string_list,
  .add_to_string_list      = layered_add_to_string_list,
  .get_key_value_list      = layered_get_key_value_list,
  .set_key_value_list      = layered_set_key_value_list,
  .contains_value          = layered_contains_value,
  .delete_value            = layered_delete_value,
  .clear_section           = layered_clear_section,
  .remove_section          = layered_remove_section,
  .remove_empty_sections   = layered_remove_empty_sections, 
};

void layered_settings_interface_init(layered_settings_interface_t* self)
{
  self->base.vtbl = &k_layered_vtable;
  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++)
    self->layers[i] = NULL;
}

void layered_settings_interface_destroy(layered_settings_interface_t* self)
{

  for (u32 i = 0; i < LAYERED_SETTINGS_NUM_LAYERS; i++)
    self->layers[i] = NULL;
  self->base.vtbl = NULL;
}

settings_interface_t* layered_settings_interface_get_layer(const layered_settings_interface_t* self,
                                                           layered_settings_layer_t layer)
{
  Assert((u32)layer < LAYERED_SETTINGS_NUM_LAYERS);
  return self->layers[layer];
}

void layered_settings_interface_set_layer(layered_settings_interface_t* self,
                                          layered_settings_layer_t layer,
                                          settings_interface_t* sif)
{
  Assert((u32)layer < LAYERED_SETTINGS_NUM_LAYERS);
  self->layers[layer] = sif;
}
