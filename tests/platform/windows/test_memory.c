/* cupid-ps1 native shared-memory, executable-storage and fault-dispatch checks. */
#include "common/memmap.h"
#include "common/error.h"
#include "common/windows/windows_util.h"
#include "util/platform/page_fault_handler.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                                           \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      fprintf(stderr, "%s:%u: %s (Windows error %lu)\n", __FILE__, (unsigned)__LINE__, #condition, \
              (unsigned long)GetLastError());                                                      \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)

static void*        s_expected_address;
static void*        s_last_pc;
static bool         s_last_write;
static unsigned     s_faults, s_chained;
static volatile u8* s_nested_address;

static void check_named_memory(void) {
  Error error = ERROR_INIT;
  char  name[128];
  CHECK(snprintf(name, sizeof(name), "cupid-platform-%lu-\xCE\xBB", (unsigned long)GetCurrentProcessId()) > 0);
  void* handle = memmap_create_shared_memory(name, HOST_PAGE_SIZE, &error);
  CHECK(handle);
  CHECK(!memmap_create_shared_memory(name, HOST_PAGE_SIZE, &error));
  CHECK(Error_is_valid(&error));
  CHECK(!memmap_create_shared_memory("\xC0", HOST_PAGE_SIZE, &error));
  wchar_t* wide = windows_utf8_to_wide(name, &error);
  CHECK(wide);
  HANDLE opened = OpenFileMappingW(FILE_MAP_READ, FALSE, wide);
  CHECK(opened);
  CloseHandle(opened);
  memmap_delete_shared_memory(name);
  memmap_destroy_shared_memory(handle);
  CHECK(!OpenFileMappingW(FILE_MAP_READ, FALSE, wide));
  free(wide);
  Error_destroy(&error);
}

static page_fault_handler_result_t fix_fault(void* pc, void* address, bool write) {
  if (address != s_expected_address) return PAGE_FAULT_HANDLER_EXECUTE_NEXT;
  s_faults++;
  s_last_pc    = pc;
  s_last_write = write;
  if (s_nested_address) *s_nested_address = 0x31;
  CHECK(memmap_mem_protect((void*)((uintptr_t)address & ~(uintptr_t)HOST_PAGE_MASK), HOST_PAGE_SIZE,
                           MEMMAP_PAGE_PROTECT_READ_WRITE));
  return PAGE_FAULT_HANDLER_CONTINUE_EXECUTION;
}

static LONG CALLBACK chained_handler(PEXCEPTION_POINTERS exception) {
  if (exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  void* address = (void*)exception->ExceptionRecord->ExceptionInformation[1];
  if (address != (const void*)s_nested_address && address != s_expected_address) return EXCEPTION_CONTINUE_SEARCH;
  s_chained++;
  CHECK(memmap_mem_protect((void*)((uintptr_t)address & ~(uintptr_t)HOST_PAGE_MASK), HOST_PAGE_SIZE,
                           MEMMAP_PAGE_PROTECT_READ_WRITE));
  return EXCEPTION_CONTINUE_EXECUTION;
}

static void check_arena(void) {
  Error error  = ERROR_INIT;
  void* handle = memmap_create_shared_memory(NULL, 8u * 1024u * 1024u, &error);
  CHECK(handle);
  u8* view = memmap_map_shared_memory(handle, 0, NULL, 8u * 1024u * 1024u, MEMMAP_PAGE_PROTECT_READ_WRITE);
  CHECK(view);
  memmap_shared_memory_area_t arena;
  memmap_shared_memory_area_init(&arena);
  CHECK(memmap_shared_memory_area_create(&arena, (size_t)1 << 32));
  const size_t offsets[] = {0, 0x200000, 0x80000000u, 0xA0000000u};
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    CHECK(memmap_shared_memory_area_map(&arena, handle, 0, arena.base_ptr + offsets[i], 0x200000,
                                        MEMMAP_PAGE_PROTECT_READ_WRITE) == arena.base_ptr + offsets[i]);
    arena.base_ptr[offsets[i] + 7] = (u8)(i + 10);
    CHECK(view[7] == i + 10);
    for (unsigned j = 0; j <= i; j++)
      CHECK(arena.base_ptr[offsets[j] + 7] == i + 10);
  }
  CHECK(arena.num_mappings == 4);
  CHECK(!memmap_shared_memory_area_map(&arena, handle, 0, arena.base_ptr + 0x200000, 4096,
                                       MEMMAP_PAGE_PROTECT_READ_WRITE));
  CHECK(!memmap_shared_memory_area_map(&arena, handle, 0, arena.base_ptr + arena.size - 4096, 8192,
                                       MEMMAP_PAGE_PROTECT_READ_WRITE));
  CHECK(!memmap_shared_memory_area_unmap(&arena, arena.base_ptr, 4096));
  CHECK(arena.num_mappings == 4);
  for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
    CHECK(memmap_shared_memory_area_unmap(&arena, arena.base_ptr + offsets[i], 0x200000));
  CHECK(arena.num_mappings == 0);
  /* Split a placeholder at page boundaries and use a non-64KB file offset. */
  u8* slot       = arena.base_ptr + 4096;
  view[4096 + 3] = 0x52;
  CHECK(memmap_shared_memory_area_map(&arena, handle, 4096, slot, 4096, MEMMAP_PAGE_PROTECT_READ_WRITE) == slot);
  CHECK(slot[3] == 0x52);
  CHECK(memmap_shared_memory_area_unmap(&arena, slot, 4096));
  /* A failed view replacement leaves the placeholder owned and reusable. */
  CHECK(!memmap_shared_memory_area_map(&arena, NULL, 0, slot, 4096, MEMMAP_PAGE_PROTECT_READ_WRITE));
  CHECK(memmap_shared_memory_area_map(&arena, handle, 0, arena.base_ptr, 0x400000, MEMMAP_PAGE_PROTECT_READ_WRITE) ==
        arena.base_ptr);
  CHECK(memmap_shared_memory_area_unmap(&arena, arena.base_ptr, 0x400000));
  void* old_base = arena.base_ptr;
  memmap_shared_memory_area_destroy(&arena);
  MEMORY_BASIC_INFORMATION info;
  CHECK(VirtualQuery(old_base, &info, sizeof(info)) && info.State == MEM_FREE);
  CHECK(!arena.base_ptr && !arena.num_mappings && !arena.platform_data);
  memmap_unmap_shared_memory(view, 8u * 1024u * 1024u);
  memmap_destroy_shared_memory(handle);
  Error_destroy(&error);
}

static void check_faults_and_jit(void) {
  Error error  = ERROR_INIT;
  void* handle = memmap_create_shared_memory(NULL, 65536, &error);
  CHECK(handle);
  u8* view  = memmap_map_shared_memory(handle, 0, NULL, 65536, MEMMAP_PAGE_PROTECT_READ_WRITE);
  u8* alias = memmap_map_shared_memory(handle, 0, NULL, 65536, MEMMAP_PAGE_PROTECT_READ_WRITE);
  CHECK(view && alias);
  CHECK(page_fault_handler_install(&error));
  page_fault_handler_set_callback(fix_fault);
  u8* code = memmap_allocate_jit_memory(HOST_PAGE_SIZE);
  CHECK(code);
  const u8 store[] = {0xC6, 0x01, 0x6A, 0xC3}; /* mov byte [rcx],6Ah; ret */
  memcpy(code, store, sizeof(store));
  memmap_flush_instruction_cache(code, sizeof(store));
  void (*write_byte)(void*);
  memcpy(&write_byte, &code, sizeof(write_byte));
  s_expected_address = view;
  CHECK(memmap_mem_protect(view, HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_READ_ONLY));
  alias[0] = 0x41; /* A protected alias must not prevent writes through this view. */
  CHECK(view[0] == 0x41 && !s_faults);
  write_byte(view);
  CHECK(view[0] == 0x6A && alias[0] == 0x6A && s_faults == 1 && s_last_pc == code && s_last_write);
  const u8 load[] = {0x0F, 0xB6, 0x01, 0xC3}; /* movzx eax,byte [rcx]; ret */
  memcpy(code, load, sizeof(load));
  memmap_flush_instruction_cache(code, sizeof(load));
  unsigned (*read_byte)(void*);
  memcpy(&read_byte, &code, sizeof(read_byte));
  CHECK(memmap_mem_protect(view, HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_NONE));
  CHECK(read_byte(view) == 0x6A && s_faults == 2 && s_last_pc == code && !s_last_write);
  PVOID later = AddVectoredExceptionHandler(0, chained_handler);
  CHECK(later);
  s_nested_address = view + HOST_PAGE_SIZE;
  CHECK(memmap_mem_protect(view, 2 * HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_READ_ONLY));
  ((volatile u8*)view)[0] = 0x43;
  CHECK(s_faults == 3 && s_chained == 1 && s_nested_address[0] == 0x31);
  s_nested_address = NULL;
  CHECK(page_fault_handler_uninstall(&error));
  CHECK(memmap_mem_protect(view, HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_READ_ONLY));
  ((volatile u8*)view)[0] = 0x44;
  CHECK(s_faults == 3 && s_chained == 2 && view[0] == 0x44);
  CHECK(RemoveVectoredExceptionHandler(later));
  CHECK(page_fault_handler_uninstall(&error));
  memmap_release_jit_memory(code, HOST_PAGE_SIZE);
  memmap_unmap_shared_memory(alias, 65536);
  memmap_unmap_shared_memory(view, 65536);
  memmap_destroy_shared_memory(handle);
  Error_destroy(&error);
}

static void check_unhandled_child(const char* executable) {
  char command[32768];
  CHECK(snprintf(command, sizeof(command), "\"%s\" --unhandled", executable) > 0);
  wchar_t* wide = windows_utf8_to_wide(command, NULL);
  CHECK(wide);
  STARTUPINFOW        startup = {.cb = sizeof(startup)};
  PROCESS_INFORMATION process = {0};
  CHECK(CreateProcessW(NULL, wide, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
  free(wide);
  CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD code = 0;
  CHECK(GetExitCodeProcess(process.hProcess, &code) && code == EXCEPTION_ACCESS_VIOLATION);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
}

int main(int argc, char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  if (argc == 2 && strcmp(argv[1], "--unhandled") == 0) {
    CHECK(page_fault_handler_install(NULL));
    void* page = memmap_allocate_jit_memory(HOST_PAGE_SIZE);
    CHECK(page && memmap_mem_protect(page, HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_NONE));
    return *(volatile u8*)page;
  }
  CHECK(memmap_get_runtime_page_size() == HOST_PAGE_SIZE);
  check_named_memory();
  check_arena();
  check_faults_and_jit();
  check_unhandled_child(argv[0]);
  puts("Windows memory checks passed: aliases, placeholders, JIT, fault context, chaining and unhandled exit");
  return 0;
}
