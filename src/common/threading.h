/*
 * APIs, mach semaphores, FreeBSD pthread_np and any per-thread priority knobs
 * that don't translate to POSIX have been dropped.
 *
 * C++ classes were collapsed into structs + module functions.  RAII
 * destructors became explicit threading_*_destroy().  std::function callbacks
 * are now plain (void(*)(void*), void*) pairs.  std::string thread names are
 * fixed 16-byte buffers because pthread_setname_np caps at 16 anyway.
 */

#ifndef CUPID_COMMON_THREADING_H
#define CUPID_COMMON_THREADING_H

#include "types.h"

#include <pthread.h>
#include <semaphore.h>

/* CPU time consumed by the calling thread, in threading_get_thread_ticks_per_second() units. */
u64 threading_get_thread_cpu_time(void);
u64 threading_get_thread_ticks_per_second(void);

/* Set the name of the current thread.  Linux truncates at 16 bytes incl NUL. */
void threading_set_name_of_current_thread(const char* name);

/* Yield the remainder of this timeslice. */
void threading_timeslice(void);

/* Sleep until an absolute CLOCK_MONOTONIC timespec. */
void threading_sleep_until(const struct timespec* abs_time);

/* Set the calling thread's nice level via setpriority(PRIO_PROCESS, 0, level).
 * Returns true on success.  Negative values require CAP_SYS_NICE. */
bool threading_set_nice_level(int level);

typedef struct {
  pthread_t native_handle;
  int       native_id;     /* gettid() result; 0 if unset */
  bool      valid;
} threading_thread_handle_t;

threading_thread_handle_t threading_thread_handle_get_for_calling_thread(void);

bool threading_thread_handle_is_valid(const threading_thread_handle_t* h);
bool threading_thread_handle_equals(const threading_thread_handle_t* a,
                                    const threading_thread_handle_t* b);

u64  threading_thread_handle_get_cpu_time(const threading_thread_handle_t* h);
bool threading_thread_handle_set_affinity(const threading_thread_handle_t* h,
                                          u64 processor_mask);
bool threading_thread_handle_is_calling_thread(const threading_thread_handle_t* h);

typedef struct {
  threading_thread_handle_t handle;
  u32                       stack_size;
  void                    (*entry_fn)(void* arg);
  void*                     entry_arg;
} threading_thread_t;

void threading_thread_init(threading_thread_t* t);
void threading_thread_destroy(threading_thread_t* t);

bool threading_thread_joinable(const threading_thread_t* t);
u32  threading_thread_get_stack_size(const threading_thread_t* t);

/* No-op once the thread is started.  Pass 0 for the platform default. */
void threading_thread_set_stack_size(threading_thread_t* t, u32 size);

bool threading_thread_start(threading_thread_t* t,
                            void (*fn)(void* arg), void* arg);
void threading_thread_detach(threading_thread_t* t);
void threading_thread_join(threading_thread_t* t);

typedef struct {
  sem_t sema;
} threading_kernel_semaphore_t;

void threading_kernel_semaphore_init(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_destroy(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_post(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_wait(threading_kernel_semaphore_t* s);
bool threading_kernel_semaphore_try_wait(threading_kernel_semaphore_t* s);

#endif /* CUPID_COMMON_THREADING_H */
