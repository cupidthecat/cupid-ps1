/* cupid-ps1 Windows shared views, reserved arenas and executable storage. */
#include "common/memmap.h"
#if defined(_WIN32)
#include "windows_util.h"
#include "common/assert.h"
#include "common/error.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct mapping_range {
  size_t                offset, size;
  bool                  mapped;
  struct mapping_range* next;
} mapping_range_t;

typedef PVOID(WINAPI* allocate2_t)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef PVOID(WINAPI* map3_t)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
typedef BOOL(WINAPI* unmap2_t)(HANDLE, PVOID, ULONG);
static allocate2_t s_allocate2;
static map3_t      s_map3;
static unmap2_t    s_unmap2;
static INIT_ONCE   s_api_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK initialize_mapping_api(PINIT_ONCE once, PVOID parameter, PVOID* context)
{
  (void)once;
  (void)parameter;
  (void)context;
  HMODULE kernel = GetModuleHandleW(L"kernelbase.dll");
  if (kernel) {
    /* memcpy avoids incompatible function-pointer casts on MinGW. */
    FARPROC address = GetProcAddress(kernel, "VirtualAlloc2");
    memcpy(&s_allocate2, &address, sizeof(s_allocate2));
    address = GetProcAddress(kernel, "MapViewOfFile3");
    memcpy(&s_map3, &address, sizeof(s_map3));
    address = GetProcAddress(kernel, "UnmapViewOfFile2");
    memcpy(&s_unmap2, &address, sizeof(s_unmap2));
  }
  return TRUE;
}

static bool mapping_api_available(void)
{
  InitOnceExecuteOnce(&s_api_once, initialize_mapping_api, NULL, NULL);
  return s_allocate2 && s_map3 && s_unmap2;
}

static DWORD page_protection(memmap_page_protect_t mode)
{
  switch (mode) {
    case MEMMAP_PAGE_PROTECT_NONE:
      return PAGE_NOACCESS;
    case MEMMAP_PAGE_PROTECT_READ_ONLY:
      return PAGE_READONLY;
    case MEMMAP_PAGE_PROTECT_READ_WRITE:
      return PAGE_READWRITE;
    case MEMMAP_PAGE_PROTECT_READ_EXECUTE:
      return PAGE_EXECUTE_READ;
    case MEMMAP_PAGE_PROTECT_READ_WRITE_EXECUTE:
      return PAGE_EXECUTE_READWRITE;
    default:
      return 0;
  }
}

u32 memmap_get_runtime_page_size(void)
{
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
}

size_t memmap_get_file_mapping_name(char* buf, size_t buf_size, const char* prefix)
{
  const int length = snprintf(buf, buf_size, "%s_%lu", prefix ? prefix : "", (unsigned long)GetCurrentProcessId());
  return length < 0 ? 0 : (size_t)length;
}

void* memmap_create_shared_memory(const char* name, size_t size, Error* error)
{
  wchar_t* wide = name && *name ? windows_utf8_to_wide(name, error) : NULL;
  if (name && *name && !wide) return NULL;
  SetLastError(ERROR_SUCCESS);
  HANDLE      handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, (DWORD)((u64)size >> 32),
                                          (DWORD)size, wide);
  const DWORD code   = GetLastError();
  free(wide);
  if (!handle || (name && *name && code == ERROR_ALREADY_EXISTS)) {
    if (handle) CloseHandle(handle);
    windows_set_error(error, "Creating shared memory", code);
    return NULL;
  }
  return handle;
}

void memmap_destroy_shared_memory(void* ptr)
{
  if (ptr) CloseHandle(ptr);
}
void memmap_delete_shared_memory(const char* name)
{
  (void)name;
}

bool memmap_mem_protect(void* baseaddr, size_t size, memmap_page_protect_t mode)
{
  DWORD previous;
  return VirtualProtect(baseaddr, size, page_protection(mode), &previous) != 0;
}

void* memmap_map_shared_memory(void* handle, size_t offset, void* baseaddr, size_t size, memmap_page_protect_t mode)
{
  if (!mapping_api_available() || !size) return NULL;
  return s_map3(handle, GetCurrentProcess(), baseaddr, offset, size, 0, page_protection(mode), NULL, 0);
}

void memmap_unmap_shared_memory(void* baseaddr, size_t size)
{
  (void)size;
  if (!UnmapViewOfFile(baseaddr)) Panic("Unmapping shared memory failed");
}

const void* memmap_get_base_address(void)
{
  return GetModuleHandleW(NULL);
}

void* memmap_allocate_jit_memory(size_t size)
{
  const uintptr_t base  = (uintptr_t)memmap_get_base_address();
  const uintptr_t step  = 64u * 1024u * 1024u;
  const uintptr_t limit = 0x80000000u;
  if (!size || size >= limit) return NULL;
  for (uintptr_t distance = step; distance < limit - size; distance += step) {
    void* result = NULL;
    if (base <= UINTPTR_MAX - distance)
      result = VirtualAlloc((void*)(base + distance), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!result && base >= distance)
      result = VirtualAlloc((void*)(base - distance), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (result) return result;
  }
  return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}

void memmap_release_jit_memory(void* ptr, size_t size)
{
  (void)size;
  if (ptr && !VirtualFree(ptr, 0, MEM_RELEASE)) Panic("Releasing executable storage failed");
}

void memmap_flush_instruction_cache(void* address, size_t size)
{
  if (!FlushInstructionCache(GetCurrentProcess(), address, size)) Panic("Flushing executable storage failed");
}

void memmap_shared_memory_area_init(memmap_shared_memory_area_t* a)
{
  memset(a, 0, sizeof(*a));
}

bool memmap_shared_memory_area_create(memmap_shared_memory_area_t* a, size_t size)
{
  if (!size || (size & HOST_PAGE_MASK) || !mapping_api_available()) return false;
  memmap_shared_memory_area_destroy(a);
  mapping_range_t* range = calloc(1, sizeof(*range));
  if (!range) return false;
  u8* base =
      s_allocate2(GetCurrentProcess(), NULL, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
  if (!base) {
    free(range);
    return false;
  }
  range->size      = size;
  a->platform_data = range;
  a->base_ptr      = base;
  a->size          = size;
  a->num_pages     = size / HOST_PAGE_SIZE;
  return true;
}

void memmap_shared_memory_area_destroy(memmap_shared_memory_area_t* a)
{
  AssertMsg(a->num_mappings == 0, "Mapped views must be removed before releasing their arena");
  mapping_range_t* range = a->platform_data;
  while (range) {
    mapping_range_t* next = range->next;
    if (!VirtualFree(a->base_ptr + range->offset, 0, MEM_RELEASE)) Panic("Releasing reserved arena failed");
    free(range);
    range = next;
  }
  memset(a, 0, sizeof(*a));
}

static bool contains_range(const memmap_shared_memory_area_t* a, const void* base, size_t size)
{
  const uintptr_t address = (uintptr_t)base, start = (uintptr_t)a->base_ptr;
  return a->base_ptr && size && !(address & HOST_PAGE_MASK) && !(size & HOST_PAGE_MASK) && address >= start &&
         address - start < a->size && size <= a->size - (address - start);
}

static bool merge_placeholders(memmap_shared_memory_area_t* a)
{
  for (mapping_range_t* range = a->platform_data; range && range->next;) {
    mapping_range_t* next = range->next;
    if (range->mapped || next->mapped) {
      range = next;
      continue;
    }
    if (!VirtualFree(a->base_ptr + range->offset, range->size + next->size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
      return false;
    range->size += next->size;
    range->next = next->next;
    free(next);
  }
  return true;
}

static bool split_placeholder(memmap_shared_memory_area_t* a, mapping_range_t* range, size_t left_size)
{
  mapping_range_t* right = malloc(sizeof(*right));
  if (!right) return false;
  if (!VirtualFree(a->base_ptr + range->offset, left_size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
    free(right);
    return false;
  }
  *right      = (mapping_range_t){range->offset + left_size, range->size - left_size, false, range->next};
  range->size = left_size;
  range->next = right;
  return true;
}

u8* memmap_shared_memory_area_map(memmap_shared_memory_area_t* a, void* file_handle, size_t file_offset, void* map_base,
                                  size_t map_size, memmap_page_protect_t mode)
{
  if (!contains_range(a, map_base, map_size) || (file_offset & HOST_PAGE_MASK) || !merge_placeholders(a)) return NULL;
  const size_t     offset = (uintptr_t)map_base - (uintptr_t)a->base_ptr;
  mapping_range_t* range  = a->platform_data;
  while (range && !(offset >= range->offset && offset - range->offset < range->size))
    range = range->next;
  if (!range || range->mapped || map_size > range->size - (offset - range->offset)) return NULL;
  if (offset != range->offset) {
    if (!split_placeholder(a, range, offset - range->offset)) return NULL;
    range = range->next;
  }
  if (map_size != range->size && !split_placeholder(a, range, map_size)) return NULL;
  u8* result = s_map3(file_handle, GetCurrentProcess(), map_base, file_offset, map_size, MEM_REPLACE_PLACEHOLDER,
                      page_protection(mode), NULL, 0);
  if (!result) return NULL;
  range->mapped = true;
  a->num_mappings++;
  return result;
}

bool memmap_shared_memory_area_unmap(memmap_shared_memory_area_t* a, void* map_base, size_t map_size)
{
  if (!contains_range(a, map_base, map_size)) return false;
  const size_t     offset = (uintptr_t)map_base - (uintptr_t)a->base_ptr;
  mapping_range_t* range  = a->platform_data;
  while (range && range->offset != offset)
    range = range->next;
  if (!range || !range->mapped || range->size != map_size) return false;
  if (!s_unmap2(GetCurrentProcess(), map_base, MEM_PRESERVE_PLACEHOLDER)) return false;
  range->mapped = false;
  a->num_mappings--;
  return merge_placeholders(a);
}
#endif
