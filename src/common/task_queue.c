#include "common/task_queue.h"
#include "common/assert.h"

#include <stdlib.h>
#include <string.h>

static void worker_thread_entry_point(void* arg);

void task_queue_init(task_queue_t* tq)
{
  memset(tq, 0, sizeof(*tq));
  pthread_mutex_init(&tq->mutex, NULL);
  pthread_cond_init(&tq->task_wait_cv, NULL);
  pthread_cond_init(&tq->tasks_done_cv, NULL);
}

void task_queue_destroy(task_queue_t* tq)
{
  task_queue_set_worker_count(tq, 0);
  Assert(tq->task_count == 0);

  free(tq->tasks);
  tq->tasks = NULL;
  tq->task_capacity = 0;

  pthread_cond_destroy(&tq->tasks_done_cv);
  pthread_cond_destroy(&tq->task_wait_cv);
  pthread_mutex_destroy(&tq->mutex);
}

/* Pop the front task; caller must hold mutex.  Releases the lock during the */
static void execute_one_task_locked(task_queue_t* tq)
{
  Assert(tq->task_count > 0);
  task_queue_entry_t entry = tq->tasks[tq->task_head];
  tq->task_head = (tq->task_head + 1u) & (tq->task_capacity - 1u);
  tq->task_count--;

  pthread_mutex_unlock(&tq->mutex);
  entry.fn(entry.ctx);
  pthread_mutex_lock(&tq->mutex);

  tq->tasks_outstanding--;
  if (tq->tasks_outstanding == 0)
    pthread_cond_broadcast(&tq->tasks_done_cv);
}

/* Caller must hold mutex.  Drains the queue while waiting for outstanding
 * work to drop to zero. */
static void wait_for_all_locked(task_queue_t* tq)
{
  while (tq->tasks_outstanding != 0) {
    while (tq->task_count != 0)
      execute_one_task_locked(tq);
    if (tq->tasks_outstanding == 0)
      break;
    pthread_cond_wait(&tq->tasks_done_cv, &tq->mutex);
  }
}

void task_queue_set_worker_count(task_queue_t* tq, u32 count)
{
  pthread_mutex_lock(&tq->mutex);

  wait_for_all_locked(tq);

  if (tq->thread_count > 0) {
    tq->threads_done = true;
    pthread_cond_broadcast(&tq->task_wait_cv);

    threading_thread_t* old_threads = tq->threads;
    u32 old_count = tq->thread_count;
    tq->threads = NULL;
    tq->thread_count = 0;

    pthread_mutex_unlock(&tq->mutex);
    for (u32 i = 0; i < old_count; i++) {
      threading_thread_join(&old_threads[i]);
      threading_thread_destroy(&old_threads[i]);
    }
    free(old_threads);
    pthread_mutex_lock(&tq->mutex);
  }

  if (count > 0) {
    tq->threads_done = false;
    tq->threads = (threading_thread_t*)calloc(count, sizeof(threading_thread_t));
    tq->thread_count = count;
    for (u32 i = 0; i < count; i++) {
      threading_thread_init(&tq->threads[i]);
      threading_thread_start(&tq->threads[i], worker_thread_entry_point, tq);
    }
  }

  pthread_mutex_unlock(&tq->mutex);
}

void task_queue_submit(task_queue_t* tq, task_queue_fn_t fn, void* ctx)
{
  pthread_mutex_lock(&tq->mutex);

  /* Grow ring buffer if full.  Always power-of-two for cheap wrap mask. */
  if (tq->task_count == tq->task_capacity) {
    const u32 new_cap = (tq->task_capacity == 0) ? 16u : (tq->task_capacity * 2u);
    task_queue_entry_t* new_tasks = (task_queue_entry_t*)malloc(new_cap * sizeof(task_queue_entry_t));
    for (u32 i = 0; i < tq->task_count; i++)
      new_tasks[i] = tq->tasks[(tq->task_head + i) & (tq->task_capacity ? (tq->task_capacity - 1u) : 0u)];
    free(tq->tasks);
    tq->tasks = new_tasks;
    tq->task_capacity = new_cap;
    tq->task_head = 0;
  }

  const u32 mask = tq->task_capacity - 1u;
  const u32 tail = (tq->task_head + tq->task_count) & mask;
  tq->tasks[tail].fn  = fn;
  tq->tasks[tail].ctx = ctx;
  tq->task_count++;
  tq->tasks_outstanding++;

  pthread_cond_signal(&tq->task_wait_cv);
  pthread_mutex_unlock(&tq->mutex);
}

void task_queue_wait_for_all(task_queue_t* tq)
{
  pthread_mutex_lock(&tq->mutex);
  wait_for_all_locked(tq);
  pthread_mutex_unlock(&tq->mutex);
}

static void worker_thread_entry_point(void* arg)
{
  task_queue_t* tq = (task_queue_t*)arg;
  threading_set_name_of_current_thread("TaskQueue Worker");

  pthread_mutex_lock(&tq->mutex);
  while (!tq->threads_done) {
    if (tq->task_count == 0) {
      pthread_cond_wait(&tq->task_wait_cv, &tq->mutex);
      continue;
    }
    execute_one_task_locked(tq);
  }
  pthread_mutex_unlock(&tq->mutex);
}
