/* cupid-ps1 native Windows threads, CPU clocks, affinity and semaphores. */
#include "common/threading.h"
#if defined(_WIN32)
#include "windows_util.h"
#include "common/assert.h"
#include "common/timer.h"
#include <process.h>
#include <stdlib.h>

void threading_timeslice(void) {
  SwitchToThread();
}

void threading_sleep_until(const struct timespec* time) {
  if (time->tv_sec < 0 || time->tv_nsec < 0 || time->tv_nsec >= 1000000000) return;
  timer_sleep_until((u64)time->tv_sec * 1000000000ull + (u64)time->tv_nsec, false);
}

bool threading_set_nice_level(int level) {
  const int priority = level < 0   ? THREAD_PRIORITY_ABOVE_NORMAL
                       : level > 0 ? THREAD_PRIORITY_BELOW_NORMAL
                                   : THREAD_PRIORITY_NORMAL;
  return SetThreadPriority(GetCurrentThread(), priority) != 0;
}

void threading_set_name_of_current_thread(const char* name) {
  typedef HRESULT(WINAPI * set_description_t)(HANDLE, PCWSTR);
  FARPROC           address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
  set_description_t set_description;
  memcpy(&set_description, &address, sizeof(set_description));
  if (!set_description) return;
  wchar_t* wide = windows_utf8_to_wide(name, NULL);
  if (!wide) return;
  set_description(GetCurrentThread(), wide);
  free(wide);
}

static u64 thread_cpu_time(HANDLE handle) {
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(handle, &created, &exited, &kernel, &user)) return 0;
  const u64 k = ((u64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
  const u64 u = ((u64)user.dwHighDateTime << 32) | user.dwLowDateTime;
  return k + u;
}

u64 threading_get_thread_cpu_time(void) {
  return thread_cpu_time(GetCurrentThread());
}
u64 threading_get_thread_ticks_per_second(void) {
  return 10000000ull;
}

threading_thread_handle_t threading_thread_handle_get_for_calling_thread(void) {
  return (threading_thread_handle_t){NULL, GetCurrentThreadId(), true};
}

bool threading_thread_handle_is_valid(const threading_thread_handle_t* handle) {
  return handle && handle->valid;
}

bool threading_thread_handle_equals(const threading_thread_handle_t* a, const threading_thread_handle_t* b) {
  return a->valid && b->valid ? a->native_id == b->native_id : a->valid == b->valid;
}

u64 threading_thread_handle_get_cpu_time(const threading_thread_handle_t* handle) {
  if (!handle->valid) return 0;
  HANDLE thread =
      handle->native_handle ? handle->native_handle : OpenThread(THREAD_QUERY_INFORMATION, FALSE, handle->native_id);
  if (!thread) return 0;
  const u64 result = thread_cpu_time(thread);
  if (!handle->native_handle) CloseHandle(thread);
  return result;
}

bool threading_thread_handle_set_affinity(const threading_thread_handle_t* handle, u64 mask) {
  if (!handle->valid) return false;
  if (!mask) {
    DWORD_PTR process_mask, system_mask;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) return false;
    mask = process_mask;
  }
  HANDLE thread =
      handle->native_handle ? handle->native_handle : OpenThread(THREAD_SET_INFORMATION, FALSE, handle->native_id);
  if (!thread) return false;
  const bool result = SetThreadAffinityMask(thread, (DWORD_PTR)mask) != 0;
  if (!handle->native_handle) CloseHandle(thread);
  return result;
}

bool threading_thread_handle_is_calling_thread(const threading_thread_handle_t* handle) {
  return handle->valid && handle->native_id == GetCurrentThreadId();
}

void threading_thread_init(threading_thread_t* thread) {
  memset(thread, 0, sizeof(*thread));
}
void threading_thread_destroy(threading_thread_t* thread) {
  AssertMsg(!thread->handle.valid, "Thread must be joined or detached");
}
bool threading_thread_joinable(const threading_thread_t* thread) {
  return thread->handle.valid;
}
u32 threading_thread_get_stack_size(const threading_thread_t* thread) {
  return thread->stack_size;
}
void threading_thread_set_stack_size(threading_thread_t* thread, u32 size) {
  AssertMsg(!thread->handle.valid, "Cannot change the stack size of a running thread");
  thread->stack_size = size;
}

typedef struct {
  void (*function)(void*);
  void* argument;
} thread_start_t;
static unsigned __stdcall thread_entry(void* parameter) {
  const thread_start_t start = *(thread_start_t*)parameter;
  free(parameter);
  start.function(start.argument);
  return 0;
}

bool threading_thread_start(threading_thread_t* thread, void (*function)(void*), void* argument) {
  AssertMsg(!thread->handle.valid, "Cannot start a running thread");
  thread_start_t* start = malloc(sizeof(*start));
  if (!start) return false;
  *start = (thread_start_t){function, argument};
  unsigned        id;
  const uintptr_t native = _beginthreadex(NULL, thread->stack_size, thread_entry, start, 0, &id);
  if (!native) {
    free(start);
    return false;
  }
  thread->handle    = (threading_thread_handle_t){(void*)native, id, true};
  thread->entry_fn  = function;
  thread->entry_arg = argument;
  return true;
}

void threading_thread_detach(threading_thread_t* thread) {
  AssertMsg(thread->handle.valid, "Cannot detach an invalid thread");
  if (!CloseHandle(thread->handle.native_handle)) Panic("Closing a detached thread handle failed");
  memset(&thread->handle, 0, sizeof(thread->handle));
}

void threading_thread_join(threading_thread_t* thread) {
  AssertMsg(thread->handle.valid, "Cannot join an invalid thread");
  if (WaitForSingleObject(thread->handle.native_handle, INFINITE) != WAIT_OBJECT_0) Panic("Joining a thread failed");
  if (!CloseHandle(thread->handle.native_handle)) Panic("Closing a joined thread handle failed");
  memset(&thread->handle, 0, sizeof(thread->handle));
}

void threading_kernel_semaphore_init(threading_kernel_semaphore_t* semaphore) {
  semaphore->sema = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
  if (!semaphore->sema) Panic("Creating a semaphore failed");
}
void threading_kernel_semaphore_destroy(threading_kernel_semaphore_t* semaphore) {
  if (!CloseHandle(semaphore->sema)) Panic("Closing a semaphore failed");
  semaphore->sema = NULL;
}
void threading_kernel_semaphore_post(threading_kernel_semaphore_t* semaphore) {
  if (!ReleaseSemaphore(semaphore->sema, 1, NULL)) Panic("Posting a semaphore failed");
}
void threading_kernel_semaphore_wait(threading_kernel_semaphore_t* semaphore) {
  if (WaitForSingleObject(semaphore->sema, INFINITE) != WAIT_OBJECT_0) Panic("Waiting for a semaphore failed");
}
bool threading_kernel_semaphore_try_wait(threading_kernel_semaphore_t* semaphore) {
  const DWORD result = WaitForSingleObject(semaphore->sema, 0);
  if (result == WAIT_OBJECT_0) return true;
  if (result != WAIT_TIMEOUT) Panic("Checking a semaphore failed");
  return false;
}
#endif
