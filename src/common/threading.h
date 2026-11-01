/*
 * Threading and synchronization helpers. Callers release objects with
 * their threading_*_destroy() functions. Callbacks carry a function pointer
 * and a context pointer. Linux names are limited to 16 bytes including NUL;
 * Windows names are converted from UTF-8 for SetThreadDescription.
 */

#ifndef CUPID_COMMON_THREADING_H
#define CUPID_COMMON_THREADING_H

#include "types.h"

#include <time.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <semaphore.h>
#endif

/* CPU time consumed by the calling thread, in threading_get_thread_ticks_per_second() units. */
u64 threading_get_thread_cpu_time(void);
u64 threading_get_thread_ticks_per_second(void);

/* Set the name of the current thread.  Linux truncates at 16 bytes incl NUL. */
void threading_set_name_of_current_thread(const char* name);

/* Yield the remainder of this timeslice. */
void threading_timeslice(void);

/* Sleep until an absolute monotonic timespec, using the host timer's epoch. */
void threading_sleep_until(const struct timespec* abs_time);

/* Set host scheduling priority. Linux uses nice levels (negative values may
 * need privilege); Windows maps negative/zero/positive to above/normal/below. */
bool threading_set_nice_level(int level);

typedef struct {
#if defined(_WIN32)
  void*     native_handle; /* Owned by threading_thread_t; NULL for a borrowed ID. */
  u32       native_id;
#else
  pthread_t native_handle;
  int       native_id;     /* gettid() result; 0 if unset */
#endif
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
#if defined(_WIN32)
  void* sema;
#else
  sem_t sema;
#endif
} threading_kernel_semaphore_t;

void threading_kernel_semaphore_init(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_destroy(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_post(threading_kernel_semaphore_t* s);
void threading_kernel_semaphore_wait(threading_kernel_semaphore_t* s);
bool threading_kernel_semaphore_try_wait(threading_kernel_semaphore_t* s);

#endif /* CUPID_COMMON_THREADING_H */
