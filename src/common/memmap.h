/*
 * Linux-only.  Win32 (VirtualAlloc/MapViewOfFile/VirtualProtect) and Apple
 * Mach VM paths dropped.  Backing store: memfd_create preferred, shm_open
 * fallback when memfd is unavailable at runtime.
 *
 * C++ namespace MemMap            -> functions prefixed memmap_
 * enum class PageProtect          -> typedef enum memmap_page_protect_t
 * class SharedMemoryMappingArea   -> struct memmap_shared_memory_area_t
 *                                    + memmap_shared_memory_area_{create,destroy,
 *                                      map,unmap,base,offset_pointer,page_pointer,
 *                                      size,num_pages}
 *
 * Lifecycle for the mapping area:
 *   memmap_shared_memory_area_t a;
 *   memmap_shared_memory_area_init(&a);
 *   if (!memmap_shared_memory_area_create(&a, size)) ...
 *   ...
 *   memmap_shared_memory_area_destroy(&a);
 *
 * BeginCodeWrite / EndCodeWrite are no-ops on Linux: W^X JIT toggling is an
 * Apple Silicon concern.  FlushInstructionCache is a no-op on x86 (coherent
 * I/D), forwards to __builtin___clear_cache otherwise.
 */

#ifndef CUPID_COMMON_MEMMAP_H
#define CUPID_COMMON_MEMMAP_H

#include "types.h"

#include <sys/mman.h>

typedef struct Error Error;

/* PROT_* flags wrapped in a typedef'd enum for stronger typing at call sites. */
typedef enum {
  MEMMAP_PAGE_PROTECT_NONE               = PROT_NONE,
  MEMMAP_PAGE_PROTECT_READ_ONLY          = PROT_READ,
  MEMMAP_PAGE_PROTECT_READ_WRITE         = PROT_READ | PROT_WRITE,
  MEMMAP_PAGE_PROTECT_READ_EXECUTE       = PROT_READ | PROT_EXEC,
  MEMMAP_PAGE_PROTECT_READ_WRITE_EXECUTE = PROT_READ | PROT_WRITE | PROT_EXEC,
} memmap_page_protect_t;

/* Returns the size of pages for the current host. */
u32 memmap_get_runtime_page_size(void);

size_t memmap_get_file_mapping_name(char* buf, size_t buf_size, const char* prefix);

/* Creates a shared memory backing store of `size` bytes.  When name is NULL or
 * empty, an anonymous mapping is created via memfd_create and is not visible
 * to other processes by name.  Returns a void* sentinel that wraps the fd
 * (cast through intptr_t); NULL on failure with details written to error. */
void* memmap_create_shared_memory(const char* name, size_t size, Error* error);

/* Closes the underlying fd returned by memmap_create_shared_memory. */
void memmap_destroy_shared_memory(void* ptr);

/* Removes a named shared memory entry (shm_unlink).  No-op when name is NULL.*/
void memmap_delete_shared_memory(const char* name);

/* Maps a region of the shared memory.  When baseaddr is non-NULL, MAP_FIXED
 * is used and any existing mapping at the address is replaced (caller must
 * have reserved the range, e.g. via memmap_shared_memory_area_t). */
void* memmap_map_shared_memory(void* handle, size_t offset, void* baseaddr,
                               size_t size, memmap_page_protect_t mode);

/* Unmaps a region previously created with memmap_map_shared_memory.  Aborts */
void memmap_unmap_shared_memory(void* baseaddr, size_t size);

/* mprotect wrapper.  Logs and returns false on failure. */
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

#if defined(CPU_ARCH_X64) || defined(CPU_ARCH_X86)
ALWAYS_INLINE static void memmap_flush_instruction_cache(void* address, size_t size)
{
  (void)address;
  (void)size;
}
#else
void memmap_flush_instruction_cache(void* address, size_t size);
#endif

/* Apple Silicon W^X helpers; no-ops on Linux on every arch.  Kept so call */
ALWAYS_INLINE static void memmap_begin_code_write(void) { }
ALWAYS_INLINE static void memmap_end_code_write(void)   { }

/* Reserves a contiguous virtual address range backed by PROT_NONE so that
 * later memmap_shared_memory_area_map calls can place file-backed views at
 * deterministic offsets.  Used to build the PS1 fastmem arena. */
typedef struct {
  u8*    base_ptr;
  size_t size;
  size_t num_pages;
  size_t num_mappings;
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
