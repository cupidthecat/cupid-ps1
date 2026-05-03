/*
 * Worker-thread pool with FIFO task queue.  C++ std::function is replaced
 * with a `(fn, ctx)` pair.  Setting worker count to 0 runs tasks inline on
 * the calling thread (via wait_for_all draining).
 */

#ifndef CUPID_COMMON_TASK_QUEUE_H
#define CUPID_COMMON_TASK_QUEUE_H

#include "common/threading.h"
#include "common/types.h"

#include <pthread.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*task_queue_fn_t)(void* ctx);

typedef struct {
  task_queue_fn_t fn;
  void*           ctx;
} task_queue_entry_t;

typedef struct {
  pthread_mutex_t      mutex;
  pthread_cond_t       task_wait_cv;
  pthread_cond_t       tasks_done_cv;

  task_queue_entry_t*  tasks;          /* ring buffer */
  u32                  task_capacity;  /* power of two, or 0 if unallocated */
  u32                  task_head;      /* dequeue position */
  u32                  task_count;     /* live entries */
  u32                  tasks_outstanding;

  threading_thread_t*  threads;
  u32                  thread_count;
  bool                 threads_done;
} task_queue_t;

void task_queue_init             (task_queue_t* tq);
void task_queue_destroy          (task_queue_t* tq);

/* Replace the worker pool with `count` threads.  Drains currently-queued
 * work first.  count == 0 means "execute submitted tasks on the calling
 * thread when wait_for_all is called". */
void task_queue_set_worker_count (task_queue_t* tq, u32 count);

void task_queue_submit           (task_queue_t* tq, task_queue_fn_t fn, void* ctx);
void task_queue_wait_for_all     (task_queue_t* tq);

#ifdef __cplusplus
}
#endif

#endif
