/* Required for gettid(), pthread_setaffinity_np, CPU_ZERO etc.  Also set in
 * the Makefile but repeated here to keep this TU self-contained. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "threading.h"

#include "assert.h"

#include <errno.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* glibc < 2.30 didn't expose gettid().  Fall back to the syscall. */
#if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 30))
static pid_t threading_gettid(void) { return (pid_t)syscall(SYS_gettid); }
#else
static pid_t threading_gettid(void) { return gettid(); }
#endif

void threading_timeslice(void)
{
  sched_yield();
}

void threading_sleep_until(const struct timespec* abs_time)
{
  /* Loop only on EINTR; other errors mean the timespec was malformed and
   * retrying won't help. */
  int res;
  do {
    res = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, abs_time, NULL);
  } while (res == EINTR);
}

bool threading_set_nice_level(int level)
{
  /* setpriority returns -1 on failure but -1 is also a legal nice value, so
   * errno must be cleared first to disambiguate. */
  errno = 0;
  if (setpriority(PRIO_PROCESS, 0, level) == -1 && errno != 0) {
    return false;
  }
  return true;
}

void threading_set_name_of_current_thread(const char* name)
{
  /* prctl is preferred over pthread_setname_np because it doesn't require a
   * thread handle and the kernel will silently truncate to 16 bytes. */
  prctl(PR_SET_NAME, name, 0, 0, 0);
}

static u64 threading_get_pthread_cpu_time(pthread_t handle, bool calling_thread)
{
  clockid_t cid;
  if (calling_thread) {
    cid = CLOCK_THREAD_CPUTIME_ID;
  } else {
    if (pthread_getcpuclockid(handle, &cid) != 0)
      return 0;
  }

  struct timespec ts;
  if (clock_gettime(cid, &ts) != 0)
    return 0;

  /* Microsecond resolution to match get_thread_ticks_per_second() == 1e6. */
  return (u64)ts.tv_sec * 1000000ULL + (u64)ts.tv_nsec / 1000ULL;
}

u64 threading_get_thread_cpu_time(void)
{
  return threading_get_pthread_cpu_time(pthread_self(), true);
}

u64 threading_get_thread_ticks_per_second(void)
{
  return 1000000ULL;
}

threading_thread_handle_t threading_thread_handle_get_for_calling_thread(void)
{
  threading_thread_handle_t h;
  h.native_handle = pthread_self();
  h.native_id     = threading_gettid();
  h.valid         = true;
  return h;
}

bool threading_thread_handle_is_valid(const threading_thread_handle_t* h)
{
  return h != NULL && h->valid;
}

bool threading_thread_handle_equals(const threading_thread_handle_t* a,
                                    const threading_thread_handle_t* b)
{
  if (!a->valid || !b->valid)
    return a->valid == b->valid;
  return pthread_equal(a->native_handle, b->native_handle) != 0;
}

u64 threading_thread_handle_get_cpu_time(const threading_thread_handle_t* h)
{
  if (!h->valid)
    return 0;
  return threading_get_pthread_cpu_time(h->native_handle, false);
}

bool threading_thread_handle_set_affinity(const threading_thread_handle_t* h,
                                          u64 processor_mask)
{
  if (!h->valid)
    return false;

  cpu_set_t set;
  CPU_ZERO(&set);

  if (processor_mask != 0) {
    /* Caller-supplied 64-bit mask, one bit per CPU. */
    for (u32 i = 0; i < 64; i++) {
      if (processor_mask & ((u64)1 << i))
        CPU_SET(i, &set);
    }
  } else {
    long num_processors = sysconf(_SC_NPROCESSORS_CONF);
    for (long i = 0; i < num_processors; i++)
      CPU_SET((int)i, &set);
  }

  return sched_setaffinity((pid_t)h->native_id, sizeof(set), &set) >= 0;
}

bool threading_thread_handle_is_calling_thread(const threading_thread_handle_t* h)
{
  if (!h->valid)
    return false;
  return pthread_equal(pthread_self(), h->native_handle) != 0;
}

/* Trampoline state.  We carry a kernel semaphore so the new thread can publish
 * its TID into the parent's handle before _start returns; the parent waits on
 * that semaphore so callers see a fully-initialised handle. */
typedef struct {
  void                       (*func)(void*);
  void*                        arg;
  threading_kernel_semaphore_t start_semaphore;
  int*                         thread_id_ptr;
} thread_proc_params_t;

static void* threading_thread_proc(void* param)
{
  thread_proc_params_t* p = (thread_proc_params_t*)param;
  void                (*func)(void*) = p->func;
  void*                  arg         = p->arg;

  *p->thread_id_ptr = threading_gettid();
  threading_kernel_semaphore_post(&p->start_semaphore);
  /* p is owned by the parent's stack via the start semaphore; once we Post,
   * the parent may free everything in p, so don't touch it again. */

  func(arg);
  return NULL;
}

void threading_thread_init(threading_thread_t* t)
{
  t->handle.native_handle = (pthread_t)0;
  t->handle.native_id     = 0;
  t->handle.valid         = false;
  t->stack_size           = 0;
  t->entry_fn             = NULL;
  t->entry_arg            = NULL;
}

void threading_thread_destroy(threading_thread_t* t)
{
  AssertMsg(!t->handle.valid, "Thread should be detached or joined at destruction");
}

bool threading_thread_joinable(const threading_thread_t* t)
{
  return t->handle.valid;
}

u32 threading_thread_get_stack_size(const threading_thread_t* t)
{
  return t->stack_size;
}

void threading_thread_set_stack_size(threading_thread_t* t, u32 size)
{
  AssertMsg(!t->handle.valid, "Can't change the stack size on a started thread");
  t->stack_size = size;
}

bool threading_thread_start(threading_thread_t* t,
                            void (*fn)(void* arg), void* arg)
{
  AssertMsg(!t->handle.valid, "Can't start an already-started thread");

  thread_proc_params_t params;
  params.func          = fn;
  params.arg           = arg;
  params.thread_id_ptr = &t->handle.native_id;
  threading_kernel_semaphore_init(&params.start_semaphore);

  pthread_attr_t attrs;
  bool has_attributes = false;
  if (t->stack_size != 0) {
    pthread_attr_init(&attrs);
    pthread_attr_setstacksize(&attrs, t->stack_size);
    has_attributes = true;
  }

  pthread_t handle;
  const int res = pthread_create(&handle, has_attributes ? &attrs : NULL,
                                 threading_thread_proc, &params);
  if (has_attributes)
    pthread_attr_destroy(&attrs);

  if (res != 0) {
    threading_kernel_semaphore_destroy(&params.start_semaphore);
    return false;
  }

  threading_kernel_semaphore_wait(&params.start_semaphore);
  threading_kernel_semaphore_destroy(&params.start_semaphore);

  t->handle.native_handle = handle;
  t->handle.valid         = true;
  t->entry_fn             = fn;
  t->entry_arg            = arg;
  return true;
}

void threading_thread_detach(threading_thread_t* t)
{
  AssertMsg(t->handle.valid, "Can't detach without a thread");
  pthread_detach(t->handle.native_handle);
  t->handle.valid     = false;
  t->handle.native_id = 0;
}

void threading_thread_join(threading_thread_t* t)
{
  AssertMsg(t->handle.valid, "Can't join without a thread");
  void* retval;
  const int res = pthread_join(t->handle.native_handle, &retval);
  if (res != 0)
    Panic("pthread_join() for thread join failed");

  t->handle.valid     = false;
  t->handle.native_id = 0;
}

void threading_kernel_semaphore_init(threading_kernel_semaphore_t* s)
{
  if (sem_init(&s->sema, 0, 0) != 0)
    Panic("sem_init() failed");
}

void threading_kernel_semaphore_destroy(threading_kernel_semaphore_t* s)
{
  sem_destroy(&s->sema);
}

void threading_kernel_semaphore_post(threading_kernel_semaphore_t* s)
{
  sem_post(&s->sema);
}

void threading_kernel_semaphore_wait(threading_kernel_semaphore_t* s)
{
  /* sem_wait can spuriously return EINTR when a signal arrives; loop. */
  for (;;) {
    if (sem_wait(&s->sema) == 0)
      return;
    if (errno != EINTR)
      return;
  }
}

bool threading_kernel_semaphore_try_wait(threading_kernel_semaphore_t* s)
{
  return sem_trywait(&s->sema) == 0;
}
