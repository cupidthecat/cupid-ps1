/* cupid-ps1 native clock, thread and concurrent fault-dispatch checks. */
#include "common/error.h"
#include "common/memmap.h"
#include "common/threading.h"
#include "common/timer.h"
#include "common/windows/windows_util.h"
#include "util/platform/page_fault_handler.h"
#include <stdatomic.h>
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

static void consume_cpu(void) {
  const timer_value_t end      = timer_get_current_value() + timer_seconds_to_value(5);
  const u64           before   = threading_get_thread_cpu_time();
  const u64           required = threading_get_thread_ticks_per_second() / 50;
  volatile unsigned   work     = 1;
  while (threading_get_thread_cpu_time() - before < required) {
    for (unsigned i = 0; i < 10000; i++)
      work = work * 1664525u + 1013904223u;
    CHECK(timer_get_current_value() < end);
  }
}

typedef struct {
  threading_kernel_semaphore_t ready, resume, finished;
  threading_thread_handle_t    parent;
  u64                          parent_cpu_before, child_cpu;
} thread_fixture_t;

static void worker(void* argument) {
  thread_fixture_t* fixture = argument;
  threading_set_name_of_current_thread("cupid-platform-\xCE\xBB");
  const threading_thread_handle_t self = threading_thread_handle_get_for_calling_thread();
  CHECK(threading_thread_handle_is_valid(&self));
  CHECK(threading_thread_handle_is_calling_thread(&self));
  CHECK(!threading_thread_handle_is_calling_thread(&fixture->parent));
  CHECK(!threading_thread_handle_equals(&self, &fixture->parent));
  CHECK(threading_thread_handle_get_cpu_time(&fixture->parent) >= fixture->parent_cpu_before);
  threading_kernel_semaphore_post(&fixture->ready);
  threading_kernel_semaphore_wait(&fixture->resume);
  consume_cpu();
  fixture->child_cpu = threading_get_thread_cpu_time();
  threading_kernel_semaphore_post(&fixture->finished);
}

static void check_thread(bool detach) {
  thread_fixture_t fixture;
  threading_kernel_semaphore_init(&fixture.ready);
  threading_kernel_semaphore_init(&fixture.resume);
  threading_kernel_semaphore_init(&fixture.finished);
  fixture.parent            = threading_thread_handle_get_for_calling_thread();
  fixture.parent_cpu_before = threading_get_thread_cpu_time();
  fixture.child_cpu         = 0;
  CHECK(!threading_kernel_semaphore_try_wait(&fixture.ready));
  threading_thread_t thread;
  threading_thread_init(&thread);
  CHECK(!threading_thread_joinable(&thread));
  threading_thread_set_stack_size(&thread, 1024 * 1024);
  CHECK(threading_thread_get_stack_size(&thread) == 1024 * 1024);
  CHECK(threading_thread_start(&thread, worker, &fixture));
  threading_kernel_semaphore_wait(&fixture.ready);
  CHECK(threading_thread_joinable(&thread));
  CHECK(!threading_thread_handle_is_calling_thread(&thread.handle));
  CHECK(threading_thread_handle_set_affinity(&thread.handle, 0));
  if (detach) {
    threading_thread_detach(&thread);
    CHECK(!threading_thread_joinable(&thread));
  }
  threading_kernel_semaphore_post(&fixture.resume);
  threading_kernel_semaphore_wait(&fixture.finished);
  if (!detach) {
    CHECK(threading_thread_handle_get_cpu_time(&thread.handle) > 0);
    threading_thread_join(&thread);
  }
  CHECK(fixture.child_cpu > 0);
  CHECK(!threading_thread_joinable(&thread));
  threading_thread_destroy(&thread);
  threading_kernel_semaphore_post(&fixture.ready);
  CHECK(threading_kernel_semaphore_try_wait(&fixture.ready));
  CHECK(!threading_kernel_semaphore_try_wait(&fixture.ready));
  threading_kernel_semaphore_destroy(&fixture.ready);
  threading_kernel_semaphore_destroy(&fixture.resume);
  threading_kernel_semaphore_destroy(&fixture.finished);
}

static u8*                         s_pages;
static atomic_uint                 s_fault_count;
static page_fault_handler_result_t concurrent_fault(void* pc, void* address, bool write) {
  if (!pc || !write || (uintptr_t)address < (uintptr_t)s_pages ||
      (uintptr_t)address - (uintptr_t)s_pages >= 8 * HOST_PAGE_SIZE)
    return PAGE_FAULT_HANDLER_EXECUTE_NEXT;
  const uintptr_t page = (uintptr_t)address & ~(uintptr_t)HOST_PAGE_MASK;
  CHECK(memmap_mem_protect((void*)page, HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_READ_WRITE));
  atomic_fetch_add_explicit(&s_fault_count, 1, memory_order_relaxed);
  return PAGE_FAULT_HANDLER_CONTINUE_EXECUTION;
}

static void write_page(void* address) {
  *(volatile u8*)address = 0x63;
}

static void check_concurrent_faults(void) {
  s_pages = memmap_allocate_jit_memory(8 * HOST_PAGE_SIZE);
  CHECK(s_pages && memmap_mem_protect(s_pages, 8 * HOST_PAGE_SIZE, MEMMAP_PAGE_PROTECT_READ_ONLY));
  CHECK(page_fault_handler_install(NULL));
  page_fault_handler_set_callback(concurrent_fault);
  threading_thread_t threads[8];
  for (unsigned i = 0; i < 8; i++) {
    threading_thread_init(&threads[i]);
    CHECK(threading_thread_start(&threads[i], write_page, s_pages + i * HOST_PAGE_SIZE));
  }
  for (unsigned i = 0; i < 8; i++) {
    threading_thread_join(&threads[i]);
    threading_thread_destroy(&threads[i]);
    CHECK(s_pages[i * HOST_PAGE_SIZE] == 0x63);
  }
  CHECK(atomic_load_explicit(&s_fault_count, memory_order_relaxed) == 8);
  CHECK(page_fault_handler_uninstall(NULL));
  memmap_release_jit_memory(s_pages, 8 * HOST_PAGE_SIZE);
}

int main(void) {
  CHECK(timer_get_frequency() == 1.0);
  CHECK(timer_seconds_to_value(1) == 1000000000);
  CHECK(timer_value_to_seconds(1000000000) == 1.0);
  timer_value_t last = timer_get_current_value();
  for (unsigned i = 0; i < 10000; i++) {
    const timer_value_t now = timer_get_current_value();
    CHECK(now >= last);
    last = now;
  }
  /* Past exact deadlines below the spin guard must not wrap into the future. */
  timer_sleep_until(1, true);
  timer_sleep_until(0, false);
  const timer_value_t start = timer_get_current_value();
  timer_nanosleep(2000000);
  CHECK(timer_get_current_value() - start >= 2000000);
  timer_sleep_until(timer_get_current_value() + 2000000, true);
  const timer_value_t   target   = timer_get_current_value() + 2000000;
  const struct timespec deadline = {(time_t)(target / 1000000000), (long)(target % 1000000000)};
  threading_sleep_until(&deadline);
  CHECK(timer_get_current_value() >= target);
  consume_cpu();
  CHECK(threading_get_thread_cpu_time() > 0 && threading_get_thread_ticks_per_second() == 10000000);
  check_thread(false);
  check_thread(true);
  check_concurrent_faults();
  puts("Windows runtime checks passed: clocks, deadlines, CPU time, join/detach, semaphores and concurrent faults");
  return 0;
}
