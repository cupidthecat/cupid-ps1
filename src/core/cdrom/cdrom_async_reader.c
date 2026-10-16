#include "cdrom_async_reader.h"

#include "common/assert.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(CDROMAsyncReader);

/* Forward decls. */
static void cdrom_async_reader_empty_buffers(cdrom_async_reader_t* r);
static void cdrom_async_reader_read_sector_non_threaded(cdrom_async_reader_t* r, cd_image_lba_t lba);
static bool cdrom_async_reader_internal_read_sector_uncached(cdrom_async_reader_t* r, cd_image_lba_t lba,
                                                             cd_image_subq_t* subq,
                                                             cdrom_async_sector_buffer_t* data);
static bool cdrom_async_reader_read_sector_into_buffer(cdrom_async_reader_t* r);
static void cdrom_async_reader_cancel_readahead(cdrom_async_reader_t* r);
static void cdrom_async_reader_worker_thread_entry(void* arg);

void cdrom_async_reader_init(cdrom_async_reader_t* r)
{
  memset(r, 0, sizeof(*r));
  pthread_mutex_init(&r->mutex, NULL);
  pthread_cond_init(&r->do_read_cv, NULL);
  pthread_cond_init(&r->notify_read_complete_cv, NULL);
  threading_thread_init(&r->read_thread);
  atomic_store(&r->shutdown_flag, true);
}

void cdrom_async_reader_destroy(cdrom_async_reader_t* r)
{
  if (!r)
    return;
  cdrom_async_reader_stop_thread(r);
  if (r->media)
  {
    cd_image_destroy(r->media);
    r->media = NULL;
  }
  free(r->buffers);
  r->buffers = NULL;
  threading_thread_destroy(&r->read_thread);
  pthread_cond_destroy(&r->notify_read_complete_cv);
  pthread_cond_destroy(&r->do_read_cv);
  pthread_mutex_destroy(&r->mutex);
}

cd_image_lba_t cdrom_async_reader_get_last_read_sector(const cdrom_async_reader_t* r)
{
  return r->buffers[atomic_load(&r->buffer_front)].lba;
}

const cdrom_async_sector_buffer_t* cdrom_async_reader_get_sector_buffer(const cdrom_async_reader_t* r)
{
  return &r->buffers[atomic_load(&r->buffer_front)].data;
}

const cd_image_subq_t* cdrom_async_reader_get_sector_subq(const cdrom_async_reader_t* r)
{
  return &r->buffers[atomic_load(&r->buffer_front)].subq;
}

u32 cdrom_async_reader_get_buffered_sector_count(const cdrom_async_reader_t* r)
{
  return atomic_load(&r->buffer_count);
}

bool cdrom_async_reader_has_buffered_sectors(const cdrom_async_reader_t* r)
{
  return atomic_load(&r->buffer_count) > 0;
}

u32 cdrom_async_reader_get_readahead_count(const cdrom_async_reader_t* r)
{
  return r->buffer_capacity;
}

bool cdrom_async_reader_has_media(const cdrom_async_reader_t* r)
{
  return r->media != NULL;
}

cd_image_t* cdrom_async_reader_get_media(cdrom_async_reader_t* r)
{
  return r->media;
}

const cd_image_t* cdrom_async_reader_get_media_const(const cdrom_async_reader_t* r)
{
  return r->media;
}

const char* cdrom_async_reader_get_media_path(const cdrom_async_reader_t* r)
{
  return r->media ? cd_image_get_path(r->media) : "";
}

bool cdrom_async_reader_is_using_thread(const cdrom_async_reader_t* r)
{
  return r->thread_started;
}

void cdrom_async_reader_start_thread(cdrom_async_reader_t* r, u32 readahead_count)
{
  if (cdrom_async_reader_is_using_thread(r))
    cdrom_async_reader_stop_thread(r);

  free(r->buffers);
  r->buffers = (cdrom_async_buffer_slot_t*)calloc(readahead_count, sizeof(cdrom_async_buffer_slot_t));
  r->buffer_capacity = readahead_count;
  cdrom_async_reader_empty_buffers(r);

  atomic_store(&r->shutdown_flag, false);
  threading_thread_start(&r->read_thread, cdrom_async_reader_worker_thread_entry, r);
  r->thread_started = true;
  INFO_LOG("Read thread started with readahead of %u sectors", readahead_count);
}

void cdrom_async_reader_stop_thread(cdrom_async_reader_t* r)
{
  if (!cdrom_async_reader_is_using_thread(r))
    return;

  pthread_mutex_lock(&r->mutex);
  atomic_store(&r->shutdown_flag, true);
  pthread_cond_signal(&r->do_read_cv);
  pthread_mutex_unlock(&r->mutex);

  threading_thread_join(&r->read_thread);
  r->thread_started = false;

  cdrom_async_reader_empty_buffers(r);
  free(r->buffers);
  r->buffers = NULL;
  r->buffer_capacity = 0;
}

void cdrom_async_reader_set_media(cdrom_async_reader_t* r, cd_image_t* media)
{
  if (cdrom_async_reader_is_using_thread(r))
    cdrom_async_reader_cancel_readahead(r);

  if (r->media)
    cd_image_destroy(r->media);
  r->media = media;

  /* In threaded mode cancel_readahead already emptied; in non-threaded mode
   * a sector from the previous disc may still sit in r->buffers[0]. Drop it
   * unconditionally so the first read after a swap goes through seek_lba on
   * the new disc. */
  cdrom_async_reader_empty_buffers(r);
  atomic_store(&r->seek_error, false);
}

cd_image_t* cdrom_async_reader_remove_media(cdrom_async_reader_t* r)
{
  if (cdrom_async_reader_is_using_thread(r))
    cdrom_async_reader_cancel_readahead(r);

  cd_image_t* prev = r->media;
  r->media = NULL;
  return prev;
}

bool cdrom_async_reader_precache(cdrom_async_reader_t* r, progress_callback_t* callback, Error* error)
{
  cdrom_async_reader_wait_for_idle(r);

  pthread_mutex_lock(&r->mutex);
  if (!r->media)
  {
    pthread_mutex_unlock(&r->mutex);
    return false;
  }
  if (cd_image_is_precached(r->media))
  {
    pthread_mutex_unlock(&r->mutex);
    return true;
  }

  /* The C port currently exposes only the in-place precache() vtable hook;
   * the C++ "fall back to memory image" path needed CDImage::CreateMemoryImage,
   * which has not been ported (and isn't on the boot critical path). */
  (void)callback;
  const cd_image_precache_result_t res = cd_image_precache(r->media, error);
  pthread_mutex_unlock(&r->mutex);

  return (res == CD_IMAGE_PRECACHE_RESULT_SUCCESS);
}

void cdrom_async_reader_queue_read_sector(cdrom_async_reader_t* r, cd_image_lba_t lba)
{
  if (!cdrom_async_reader_is_using_thread(r))
  {
    cdrom_async_reader_read_sector_non_threaded(r, lba);
    return;
  }

  const u32 buffer_count = atomic_load(&r->buffer_count);
  if (buffer_count > 0)
  {
    const u32 buffer_front = atomic_load(&r->buffer_front);
    if (r->buffers[buffer_front].lba == lba)
    {
      DEBUG_LOG("Skipping re-reading same sector %u", lba);
      return;
    }

    /* Did we readahead to the correct sector? */
    const u32 next_buffer = (buffer_front + 1u) % r->buffer_capacity;
    if (buffer_count > 1 && r->buffers[next_buffer].lba == lba)
    {
      /* Readahead hit; advance the front pointer and kick the worker so
       * it can refill the slot we just consumed. */
      DEBUG_LOG("Readahead buffer hit for sector %u", lba);
      atomic_store(&r->buffer_front, next_buffer);
      atomic_fetch_sub(&r->buffer_count, 1u);
      atomic_store(&r->can_readahead, true);
      pthread_cond_signal(&r->do_read_cv);
      return;
    }
  }

  /* Toss the readahead and start fresh. */
  DEBUG_LOG("Readahead buffer miss, queueing seek to %u", lba);
  pthread_mutex_lock(&r->mutex);
  atomic_store(&r->next_position_set, true);
  atomic_store(&r->next_position, lba);
  pthread_cond_signal(&r->do_read_cv);
  pthread_mutex_unlock(&r->mutex);
}

bool cdrom_async_reader_read_sector_uncached(cdrom_async_reader_t* r, cd_image_lba_t lba,
                                              cd_image_subq_t* subq,
                                             cdrom_async_sector_buffer_t* data) 
{
  if (!cdrom_async_reader_is_using_thread(r))
    return cdrom_async_reader_internal_read_sector_uncached(r, lba, subq, data);

  pthread_mutex_lock(&r->mutex);

  /* Wait until the read thread is idle. */
  while (atomic_load(&r->is_reading))
    pthread_cond_wait(&r->notify_read_complete_cv, &r->mutex);

  /* Read while the lock is held so the worker has to wait. */
  const cd_image_lba_t prev_lba = cd_image_get_position_on_disc(r->media);
  const bool result = cdrom_async_reader_internal_read_sector_uncached(r, lba, subq, data);
  if (!cd_image_seek_lba(r->media, prev_lba))
  {
    ERROR_LOG("Failed to re-seek to cached position %u", prev_lba);
    atomic_store(&r->can_readahead, false);
  }

  pthread_mutex_unlock(&r->mutex);
  return result;
}

static bool cdrom_async_reader_internal_read_sector_uncached(cdrom_async_reader_t* r, cd_image_lba_t lba,
                                                              cd_image_subq_t* subq,
                                                             cdrom_async_sector_buffer_t* data) 
{
  if (cd_image_get_position_on_disc(r->media) != lba && !cd_image_seek_lba(r->media, lba))
  {
    WARNING_LOG("Seek to LBA %u failed", lba);
    return false;
  }

  if (!cd_image_read_raw_sector(r->media, data, subq))
  {
    WARNING_LOG("Read of LBA %u failed", lba);
    return false;
  }

  return true;
}

bool cdrom_async_reader_wait_for_read_to_complete(cdrom_async_reader_t* r)
{
  /* Fast path: no pending seek and we already have a buffered sector. */
  if (!atomic_load(&r->next_position_set) && atomic_load(&r->buffer_count) > 0)
  {
    TRACE_LOG("Returning sector %u", r->buffers[atomic_load(&r->buffer_front)].lba);
    return r->buffers[atomic_load(&r->buffer_front)].result;
  }

  DEBUG_LOG("Sector read pending, waiting");

  pthread_mutex_lock(&r->mutex);
  while (!((atomic_load(&r->buffer_count) > 0 || atomic_load(&r->seek_error)) &&
           !atomic_load(&r->next_position_set)))
  {
    pthread_cond_wait(&r->notify_read_complete_cv, &r->mutex);
  }
  if (atomic_load(&r->seek_error))
  {
    atomic_store(&r->seek_error, false);
    pthread_mutex_unlock(&r->mutex);
    return false;
  }

  const u32 front = atomic_load(&r->buffer_front);
  const bool result = r->buffers[front].result;
  pthread_mutex_unlock(&r->mutex);

  TRACE_LOG("Returning sector %u after waiting", r->buffers[front].lba);
  return result;
}

void cdrom_async_reader_wait_for_idle(cdrom_async_reader_t* r)
{
  if (!cdrom_async_reader_is_using_thread(r))
    return;

  pthread_mutex_lock(&r->mutex);
  while (atomic_load(&r->is_reading) || atomic_load(&r->next_position_set))
    pthread_cond_wait(&r->notify_read_complete_cv, &r->mutex);
  pthread_mutex_unlock(&r->mutex);
}

static void cdrom_async_reader_empty_buffers(cdrom_async_reader_t* r)
{
  atomic_store(&r->buffer_front, 0u);
  atomic_store(&r->buffer_back, 0u);
  atomic_store(&r->buffer_count, 0u);
}

/* Caller MUST hold r->mutex.  Releases the lock around the actual read so
 * other threads can interact with the reader (e.g. to issue a new seek). */
static bool cdrom_async_reader_read_sector_into_buffer(cdrom_async_reader_t* r)
{
  const u32 slot = atomic_load(&r->buffer_back);
  atomic_store(&r->buffer_back, (slot + 1u) % r->buffer_capacity);

  cdrom_async_buffer_slot_t* buffer = &r->buffers[slot];
  buffer->lba = cd_image_get_position_on_disc(r->media);
  atomic_store(&r->is_reading, true);
  pthread_mutex_unlock(&r->mutex);

  TRACE_LOG("Reading LBA %u...", buffer->lba);

  buffer->result = cd_image_read_raw_sector(r->media, &buffer->data, &buffer->subq);
  if (!buffer->result)
    ERROR_LOG("Read of LBA %u failed", buffer->lba);

  pthread_mutex_lock(&r->mutex);
  atomic_store(&r->is_reading, false);
  atomic_fetch_add(&r->buffer_count, 1u);
  pthread_cond_broadcast(&r->notify_read_complete_cv);
  return true;
}

static void cdrom_async_reader_read_sector_non_threaded(cdrom_async_reader_t* r, cd_image_lba_t lba)
{
  /* Allocate a single-slot buffer for non-threaded mode. */
  if (r->buffer_capacity != 1)
  {
    free(r->buffers);
    r->buffers = (cdrom_async_buffer_slot_t*)calloc(1, sizeof(cdrom_async_buffer_slot_t));
    r->buffer_capacity = 1;
  }
  atomic_store(&r->seek_error, false);
  cdrom_async_reader_empty_buffers(r);

  if (cd_image_get_position_on_disc(r->media) != lba && !cd_image_seek_lba(r->media, lba))
  {
    WARNING_LOG("Seek to LBA %u failed", lba);
    atomic_store(&r->seek_error, true);
    return;
  }

  cdrom_async_buffer_slot_t* buffer = &r->buffers[0];
  buffer->lba = cd_image_get_position_on_disc(r->media);

  TRACE_LOG("Reading LBA %u...", buffer->lba);

  buffer->result = cd_image_read_raw_sector(r->media, &buffer->data, &buffer->subq);
  if (!buffer->result)
    ERROR_LOG("Read of LBA %u failed", buffer->lba);

  atomic_fetch_add(&r->buffer_count, 1u);
}

static void cdrom_async_reader_cancel_readahead(cdrom_async_reader_t* r)
{
  DEV_LOG("Cancelling readahead");

  pthread_mutex_lock(&r->mutex);

  /* Wait until the read thread is idle. */
  while (atomic_load(&r->is_reading))
    pthread_cond_wait(&r->notify_read_complete_cv, &r->mutex);

  /* Prevent it from doing any more when it re-acquires the lock. */
  atomic_store(&r->can_readahead, false);
  cdrom_async_reader_empty_buffers(r);
  pthread_mutex_unlock(&r->mutex);
}

static void cdrom_async_reader_worker_thread_entry(void* arg)
{
  cdrom_async_reader_t* r = (cdrom_async_reader_t*)arg;

  threading_set_name_of_current_thread("CDROM Reader");

  pthread_mutex_lock(&r->mutex);

  for (;;)
  {
    while (!atomic_load(&r->shutdown_flag) && !atomic_load(&r->next_position_set) &&
           !atomic_load(&r->can_readahead))
    {
      pthread_cond_wait(&r->do_read_cv, &r->mutex);
    }
    if (atomic_load(&r->shutdown_flag))
      break;

    for (;;)
    {
      if (atomic_load(&r->next_position_set))
      {
        /* Discard buffers, we're seeking to a new location. */
        const cd_image_lba_t seek_location = atomic_load(&r->next_position);
        cdrom_async_reader_empty_buffers(r);
        atomic_store(&r->next_position_set, false);
        atomic_store(&r->seek_error, false);
        atomic_store(&r->is_reading, true);
        pthread_mutex_unlock(&r->mutex);

        /* Seek without lock held in case it takes time. */
        DEBUG_LOG("Seeking to LBA %u...", seek_location);
        const bool seek_result =
          (cd_image_get_position_on_disc(r->media) == seek_location ||
           cd_image_seek_lba(r->media, seek_location));

        pthread_mutex_lock(&r->mutex);
        atomic_store(&r->is_reading, false);

        /* Did another request come in?  Abort if so. */
        if (atomic_load(&r->next_position_set))
          continue;

        /* Did we fail the seek? */
        if (!seek_result)
        {
          /* Add the error result, and don't try to read ahead. */
          WARNING_LOG("Seek to LBA %u failed", seek_location);
          atomic_store(&r->seek_error, true);
          pthread_cond_broadcast(&r->notify_read_complete_cv);
          break;
        }

        atomic_store(&r->can_readahead, true);
      }

      if (!atomic_load(&r->can_readahead))
        break;

      /* Read as many sectors as we have space for. */
      DEBUG_LOG("Reading ahead %u sectors...", r->buffer_capacity - atomic_load(&r->buffer_count));
      while (atomic_load(&r->buffer_count) < r->buffer_capacity)
      {
        if (atomic_load(&r->next_position_set))
          break; /* Seek request came in mid-readahead, bail. */
        if (!cdrom_async_reader_read_sector_into_buffer(r))
          break;
      }

      /* Readahead buffer is full or errored at this point. */
      atomic_store(&r->can_readahead, false);
      break;
    }
  }

  pthread_mutex_unlock(&r->mutex);
}
