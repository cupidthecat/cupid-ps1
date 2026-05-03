/*
 *

 * Linux mmap / mprotect / memfd_create only.  Windows VirtualAlloc and Apple
 * Mach VM paths removed entirely.
 */
#include "memmap.h"

#include "align.h"
#include "assert.h"
#include "error.h"
#include "log.h"
#include "small_string.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/* Older glibc/kernel headers may not define MAP_FIXED_NOREPLACE.  The kernel
 * accepts the flag on 4.17+; on older kernels mmap will return EINVAL and
 * we'll fall back to the next candidate address. */
#if !defined(MAP_FIXED_NOREPLACE)
#define MAP_FIXED_NOREPLACE 0x100000
#endif

LOG_CHANNEL(MemMap);

/* Forward decl for the internal "place RWX at hint" helper. */
static void* memmap_allocate_jit_memory_at(const void* addr, size_t size);

u32 memmap_get_runtime_page_size(void)
{
  static u32 cached = 0;
  if (LIKELY(cached != 0))
    return cached;

  long res = sysconf(_SC_PAGESIZE);
  cached = (res > 0) ? (u32)res : 0;
  return cached;
}

bool memmap_mem_protect(void* baseaddr, size_t size, memmap_page_protect_t mode)
{
  DebugAssertMsg((size & (HOST_PAGE_SIZE - 1)) == 0, "Size is page aligned");

  if (UNLIKELY(mprotect(baseaddr, size, (int)mode) != 0))
  {
    ERROR_LOG("mprotect() for %zu at %p failed: %d", size, baseaddr, errno);
    return false;
  }
  return true;
}

size_t memmap_get_file_mapping_name(char* buf, size_t buf_size, const char* prefix)
{
  const unsigned pid = (unsigned)getpid();
  int n = snprintf(buf, buf_size, "%s_%u", prefix ? prefix : "", pid);
  return (n < 0) ? 0 : (size_t)n;
}

/* memfd_create lives in <sys/mman.h> on glibc 2.27+, but we still guard with
 * a weak decl so building against older sysroots doesn't fail.  At runtime
 * we fall through to shm_open if memfd_create returns -1 with ENOSYS. */
#if defined(__linux__)
extern int memfd_create(const char* name, unsigned int flags) __attribute__((weak));
#endif

void* memmap_create_shared_memory(const char* name, size_t size, Error* error)
{
  const bool is_anonymous = (!name || *name == 0);
  int fd = -1;

  if (is_anonymous && memfd_create)
  {
    fd = memfd_create("cupid-ps1", 0);
    if (fd < 0 && errno != ENOSYS)
    {
      Error_set_errno_prefix(error, "memfd_create() failed: ", errno);
      return NULL;
    }
  }

  if (fd < 0)
  {
    if (is_anonymous)
    {
      /* Synthesize a unique name for the anonymous case so shm_open is happy.
       * Unlinked immediately below. */
      char tmpname[64];
      snprintf(tmpname, sizeof(tmpname), "/cupid-ps1_%u_%lx", (unsigned)getpid(),
               (unsigned long)(uintptr_t)&size);
      fd = shm_open(tmpname, O_CREAT | O_EXCL | O_RDWR, 0600);
      if (fd < 0)
      {
        Error_set_errno_prefix(error, "shm_open() failed: ", errno);
        return NULL;
      }
      shm_unlink(tmpname);
    }
    else
    {
      fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
      if (fd < 0)
      {
        Error_set_errno_prefix(error, "shm_open() failed: ", errno);
        return NULL;
      }
    }
  }

  /* fallocate ensures the kernel reserves backing storage now, avoiding a
   * later SIGBUS when a write tries to allocate a page on a full disk/tmpfs.
   * ftruncate alone wouldn't reserve blocks. */
  if (fallocate(fd, 0, 0, (off_t)size) < 0)
  {
    /* Some filesystems (or memfd on very old kernels) don't support
     * fallocate; fall back to ftruncate so we still get the right size. */
    if (errno == EOPNOTSUPP || errno == ENOSYS)
    {
      if (ftruncate(fd, (off_t)size) < 0)
      {
        tiny_string_t pfx;
        tiny_string_init(&pfx);
        tiny_string_make_sprintf(&pfx, "ftruncate(%zu) failed: ", size);
        Error_set_errno_prefix(error, small_string_c_str(&pfx.s), errno);
        small_string_destroy(&pfx.s);
        close(fd);
        if (!is_anonymous)
          shm_unlink(name);
        return NULL;
      }
    }
    else
    {
      tiny_string_t pfx;
      tiny_string_init(&pfx);
      tiny_string_make_sprintf(&pfx, "fallocate(%zu) failed: ", size);
      Error_set_errno_prefix(error, small_string_c_str(&pfx.s), errno);
      small_string_destroy(&pfx.s);
      close(fd);
      if (!is_anonymous)
        shm_unlink(name);
      return NULL;
    }
  }

  return (void*)(intptr_t)fd;
}

void memmap_destroy_shared_memory(void* ptr)
{
  close((int)(intptr_t)ptr);
}

void memmap_delete_shared_memory(const char* name)
{
  if (name)
    shm_unlink(name);
}

void* memmap_map_shared_memory(void* handle, size_t offset, void* baseaddr,
                               size_t size, memmap_page_protect_t mode)
{
  /* MAP_FIXED is required when baseaddr is provided so we land in a
   * pre-reserved slot inside a memmap_shared_memory_area_t. */
  const int flags = (baseaddr != NULL) ? (MAP_SHARED | MAP_FIXED) : MAP_SHARED;
  void* ptr = mmap(baseaddr, size, (int)mode, flags, (int)(intptr_t)handle, (off_t)offset);
  if (ptr == MAP_FAILED)
    return NULL;
  return ptr;
}

void memmap_unmap_shared_memory(void* baseaddr, size_t size)
{
  if (munmap(baseaddr, size) != 0)
    Panic("Failed to unmap shared memory");
}

const void* memmap_get_base_address(void)
{
  Dl_info info;
  if (dladdr((const void*)&memmap_get_base_address, &info) == 0)
  {
    ERROR_LOG("dladdr() failed");
    return NULL;
  }
  return info.dli_fbase;
}

static void* memmap_allocate_jit_memory_at(const void* addr, size_t size)
{
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

  if (addr)
    flags |= MAP_FIXED_NOREPLACE;

  void* ptr = mmap((void*)addr, size, PROT_READ | PROT_WRITE | PROT_EXEC, flags, -1, 0);
  if (ptr == MAP_FAILED)
  {
    if (!addr)
      ERROR_LOG("mmap(RWX, %zu) for internal buffer failed: %d", size, errno);
    return NULL;
  }

  /* Older kernels without MAP_FIXED_NOREPLACE silently ignore the hint and
   * return whatever they found; reject that so the caller keeps walking
   * candidate addresses. */
  if (addr && ptr != addr)
  {
    if (munmap(ptr, size) != 0)
      ERROR_LOG("Failed to munmap() incorrectly hinted allocation: %d", errno);
    return NULL;
  }
  return ptr;
}

void* memmap_allocate_jit_memory(size_t size)
{
  const u8* base = (const u8*)AlignDownPow2((uintptr_t)memmap_get_base_address(), HOST_PAGE_SIZE);
  u8* ptr = NULL;

#if defined(CPU_ARCH_X64)
  static const size_t assume_binary_size = 64u * 1024u * 1024u;
  static const size_t step              = 64u * 1024u * 1024u;
  static const size_t max_displacement  = 0x80000000u;
#elif defined(CPU_ARCH_ARM64) || defined(CPU_ARCH_RISCV64) || defined(CPU_ARCH_LOONGARCH64)
  static const size_t assume_binary_size = 16u * 1024u * 1024u;
  static const size_t step              = 8u  * 1024u * 1024u;
  /* Technically 4GB on ARM64; cap at 1GB to bound the search loop. */
  static const size_t max_displacement  = 1024u * 1024u * 1024u;
#elif defined(CPU_ARCH_ARM32)
  static const size_t assume_binary_size = 8u * 1024u * 1024u;
  static const size_t step              = 2u * 1024u * 1024u;
  static const size_t max_displacement  = 32u * 1024u * 1024u;
#else
#  error Unhandled architecture for memmap_allocate_jit_memory.
#endif

  Assert(size <= max_displacement);
  const size_t max_displacement_from_start = max_displacement - size;

  const ptrdiff_t base_offset = (ptrdiff_t)(uintptr_t)base;
  const ptrdiff_t down_clamp  = (base_offset < (ptrdiff_t)max_displacement_from_start)
                                   ? base_offset
                                   : (ptrdiff_t)max_displacement_from_start;
  const u8* min_address = base - down_clamp;
  const u8* max_address = base + max_displacement_from_start;
  VERBOSE_LOG("Base address: %p", (const void*)base);
  VERBOSE_LOG("Acceptable address range: %p - %p", (const void*)min_address, (const void*)max_address);

  /* Walk forward from base + assumed binary size in `step` increments. */
  for (const u8* current_address = base + assume_binary_size;; current_address += step)
  {
    VERBOSE_LOG("Trying %p (displacement 0x%lx)", (const void*)current_address,
                (long)(current_address - base));
    ptr = (u8*)memmap_allocate_jit_memory_at(current_address, size);
    if (ptr)
      break;

    const uintptr_t cur = (uintptr_t)current_address;
    if ((cur + step) > (uintptr_t)max_address || (cur + step) < cur)
      break;
  }

  /* Try below base if forward scan failed (rare). */
  if (!ptr && (uintptr_t)base >= step)
  {
    for (const u8* current_address = base - step;; current_address -= step)
    {
      VERBOSE_LOG("Trying %p (displacement 0x%lx)", (const void*)current_address,
                  (long)(base - current_address));
      ptr = (u8*)memmap_allocate_jit_memory_at(current_address, size);
      if (ptr)
        break;

      const uintptr_t cur = (uintptr_t)current_address;
      if ((cur - step) < (uintptr_t)min_address || (cur - step) > cur)
        break;
    }
  }

  if (!ptr)
  {
#if defined(CPU_ARCH_X64)
    ERROR_LOG("Failed to allocate JIT buffer in range, expect crashes.");
#endif
    ptr = (u8*)memmap_allocate_jit_memory_at(NULL, size);
    if (!ptr)
      return NULL;
  }

  const ptrdiff_t delta = (ptr >= base) ? (ptr - base) : (base - ptr);
  INFO_LOG("Allocated JIT buffer of size %zu at %p (0x%lx bytes / %ld MB away)", size,
           (void*)ptr, (long)delta, (long)((delta + (1024 * 1024 - 1)) / (1024 * 1024)));
  return ptr;
}

void memmap_release_jit_memory(void* ptr, size_t size)
{
  if (munmap(ptr, size) != 0)
    ERROR_LOG("Failed to free code pointer %p", ptr);
}

#if !defined(CPU_ARCH_X64) && !defined(CPU_ARCH_X86)
void memmap_flush_instruction_cache(void* address, size_t size)
{
  __builtin___clear_cache((char*)address, (char*)address + size);
}
#endif

void memmap_shared_memory_area_init(memmap_shared_memory_area_t* a)
{
  a->base_ptr     = NULL;
  a->size         = 0;
  a->num_pages    = 0;
  a->num_mappings = 0;
}

bool memmap_shared_memory_area_create(memmap_shared_memory_area_t* a, size_t size)
{
  AssertMsg(IsAlignedPow2(size, HOST_PAGE_SIZE), "Size is page aligned");
  memmap_shared_memory_area_destroy(a);

  /* Reserve the range with PROT_NONE so nothing else grabs it; later Map()
   * calls will MAP_FIXED over individual sub-ranges with the desired prot. */
  void* alloc = mmap(NULL, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (alloc == MAP_FAILED)
    return false;

  a->base_ptr  = (u8*)alloc;
  a->size      = size;
  a->num_pages = size >> HOST_PAGE_SHIFT;
  return true;
}

void memmap_shared_memory_area_destroy(memmap_shared_memory_area_t* a)
{
  AssertMsg(a->num_mappings == 0, "No mappings left");

  if (a->base_ptr && munmap(a->base_ptr, a->size) != 0)
    Panic("Failed to release shared memory area");

  a->base_ptr  = NULL;
  a->size      = 0;
  a->num_pages = 0;
}

u8* memmap_shared_memory_area_map(memmap_shared_memory_area_t* a, void* file_handle,
                                   size_t file_offset, void* map_base, size_t map_size,
                                  memmap_page_protect_t mode) 
{
  DebugAssert((u8*)map_base >= a->base_ptr && (u8*)map_base < (a->base_ptr + a->size));

  /* MAP_FIXED here is intentional and safe: we own the surrounding range
   * (PROT_NONE reservation in _create) and want the kernel to overlay this
   * sub-range with the file mapping at the exact requested address. */
  void* ptr = mmap(map_base, map_size, (int)mode, MAP_SHARED | MAP_FIXED,
                   (int)(intptr_t)file_handle, (off_t)file_offset);
  if (ptr == MAP_FAILED)
    return NULL;

  a->num_mappings++;
  return (u8*)ptr;
}

bool memmap_shared_memory_area_unmap(memmap_shared_memory_area_t* a, void* map_base, size_t map_size)
{
  DebugAssert((u8*)map_base >= a->base_ptr && (u8*)map_base < (a->base_ptr + a->size));

  /* Replace the live mapping with PROT_NONE anonymous so the surrounding
   * reservation stays continuous (a plain munmap would punch a hole that
   * other allocations could later steal). */
  if (mmap(map_base, map_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    return false;

  a->num_mappings--;
  return true;
}
