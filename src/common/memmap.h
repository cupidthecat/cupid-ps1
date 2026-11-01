/*
 * Shared-memory mappings and executable storage. Linux uses memfd_create or
 * shm_open; Windows uses pagefile-backed sections and placeholder arenas.
 *
 * Lifecycle for the mapping area:
 *   memmap_shared_memory_area_t a;
 *   memmap_shared_memory_area_init(&a);
 *   if (!memmap_shared_memory_area_create(&a, size)) ...
 *   ...
 *   memmap_shared_memory_area_destroy(&a);
 *
 * Code-write hooks are no-ops. Windows explicitly flushes the instruction
 * cache; Linux x86 uses coherent caches without an explicit flush.
 */

#ifndef CUPID_COMMON_MEMMAP_H
#define CUPID_COMMON_MEMMAP_H

#include "types.h"

typedef struct Error Error;

/* Stable permission bits; each platform translates them to its host API. */
typedef enum {
  MEMMAP_PAGE_PROTECT_NONE               = 0,
  MEMMAP_PAGE_PROTECT_READ_ONLY          = 1,
  MEMMAP_PAGE_PROTECT_READ_WRITE         = 3,
  MEMMAP_PAGE_PROTECT_READ_EXECUTE       = 5,
  MEMMAP_PAGE_PROTECT_READ_WRITE_EXECUTE = 7,
} memmap_page_protect_t;

/* Returns the size of pages for the current host. */
u32 memmap_get_runtime_page_size(void);

size_t memmap_get_file_mapping_name(char* buf, size_t buf_size, const char* prefix);

/* Creates a shared backing store. NULL/empty names select anonymous storage.
 * The opaque handle owns an fd on Linux or a section handle on Windows.
 * NULL indicates failure, with details written to error. */
void* memmap_create_shared_memory(const char* name, size_t size, Error* error);

/* Releases the handle returned by memmap_create_shared_memory. */
void memmap_destroy_shared_memory(void* ptr);

/* Removes a named Linux entry. Windows names disappear with the final handle. */
void memmap_delete_shared_memory(const char* name);

/* Maps a standalone shared view, optionally at an exact available address.
 * Use shared_memory_area_map to replace owned arena reservations. Linux
 * standalone mappings also support replacing a reserved range. */
void* memmap_map_shared_memory(void* handle, size_t offset, void* baseaddr,
                               size_t size, memmap_page_protect_t mode);

/* Unmaps a region previously created with memmap_map_shared_memory.  Aborts */
void memmap_unmap_shared_memory(void* baseaddr, size_t size);

/* Changes access permissions. Returns false on failure. */
bool memmap_mem_protect(void* baseaddr, size_t size, memmap_page_protect_t mode);

/* Returns the base address of the current process module.  Useful for
 * placing JIT memory within branch range of the executable. */
const void* memmap_get_base_address(void);

/* Allocates a JIT (RWX) region within branch range of the process base when
 * possible.  Falls back to an unconstrained allocation when no nearby slot
 * exists.  Returns NULL on failure. */
void* memmap_allocate_jit_memory(size_t size);

/* Releases JIT memory previously returned by memmap_allocate_jit_memory. */
void memmap_release_jit_memory(void* ptr, size_t size);

#if (defined(CPU_ARCH_X64) || defined(CPU_ARCH_X86)) && !defined(_WIN32)
ALWAYS_INLINE static void memmap_flush_instruction_cache(void* address, size_t size)
{
  (void)address;
  (void)size;
}
#else
void memmap_flush_instruction_cache(void* address, size_t size);
#endif

/* Code-write lifecycle hooks do not change page permissions. */
ALWAYS_INLINE static void memmap_begin_code_write(void) { }
ALWAYS_INLINE static void memmap_end_code_write(void)   { }

/* Owns a contiguous address reservation for views at deterministic offsets.
 * Unmapped ranges remain inaccessible and reserved until destruction. */
typedef struct {
  u8*    base_ptr;
  size_t size;
  size_t num_pages;
  size_t num_mappings;
#if defined(_WIN32)
  void* platform_data; /* Placeholder and mapped-view ranges. */
#endif
} memmap_shared_memory_area_t;

void memmap_shared_memory_area_init(memmap_shared_memory_area_t* a);

bool memmap_shared_memory_area_create (memmap_shared_memory_area_t* a, size_t size);
void memmap_shared_memory_area_destroy(memmap_shared_memory_area_t* a);

u8*  memmap_shared_memory_area_map  (memmap_shared_memory_area_t* a, void* file_handle,
                                     size_t file_offset, void* map_base, size_t map_size,
                                     memmap_page_protect_t mode);
bool memmap_shared_memory_area_unmap(memmap_shared_memory_area_t* a, void* map_base,
                                     size_t map_size);

ALWAYS_INLINE static size_t memmap_shared_memory_area_size(const memmap_shared_memory_area_t* a)
{
  return a->size;
}
ALWAYS_INLINE static size_t memmap_shared_memory_area_num_pages(const memmap_shared_memory_area_t* a)
{
  return a->num_pages;
}
ALWAYS_INLINE static u8* memmap_shared_memory_area_base(const memmap_shared_memory_area_t* a)
{
  return a->base_ptr;
}
ALWAYS_INLINE static u8* memmap_shared_memory_area_offset_pointer(const memmap_shared_memory_area_t* a, size_t offset)
{
  return a->base_ptr + offset;
}
ALWAYS_INLINE static u8* memmap_shared_memory_area_page_pointer(const memmap_shared_memory_area_t* a, size_t page)
{
  return a->base_ptr + (page << HOST_PAGE_SHIFT);
}

#endif /* CUPID_COMMON_MEMMAP_H */
