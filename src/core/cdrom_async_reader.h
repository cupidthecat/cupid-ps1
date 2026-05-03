/*
 * Background CD reader.  Owns the cd_image_t* pointer for its lifetime.
 *
 * pthread + 2x condition variable + mutex; Linux-only.  Buffers are a
 * heap-allocated array of BufferSlot whose size is fixed at thread-start
 * time (start_thread takes readahead_count).
 *
 * Atomic indices are plain u32; all reads/writes happen either inside
 * the mutex or via __atomic_load_n / __atomic_store_n with
 * memory_order_seq_cst.
 */

#ifndef CUPID_CORE_CDROM_ASYNC_READER_H
#define CUPID_CORE_CDROM_ASYNC_READER_H

#include "common/threading.h"
#include "common/types.h"
#include "util/cd_image.h"

#include <pthread.h>
#include <stdatomic.h>

typedef struct Error Error;
typedef struct progress_callback progress_callback_t;

typedef u8 cdrom_async_sector_buffer_t[CD_IMAGE_RAW_SECTOR_SIZE];

typedef struct {
  cd_image_lba_t              lba;
  cd_image_subq_t             subq;
  cdrom_async_sector_buffer_t data;
  bool                        result;
} cdrom_async_buffer_slot_t;

typedef struct {
  /* Owned disc image, NULL if no media. */
  cd_image_t* media;

  pthread_mutex_t mutex;
  pthread_cond_t  do_read_cv;
  pthread_cond_t  notify_read_complete_cv;

  threading_thread_t read_thread;
  bool               thread_started;

  /* All u32/bool flags below are accessed under m_mutex except where noted as
   * "atomic"; for those we use the GCC __atomic_* builtins with seq_cst
   * semantics, matching std::atomic<T>'s default. */
  _Atomic cd_image_lba_t next_position;
  _Atomic bool           next_position_set;
  _Atomic bool           shutdown_flag;

  _Atomic bool is_reading;
  _Atomic bool can_readahead;
  _Atomic bool seek_error;

  cdrom_async_buffer_slot_t* buffers;
  u32                        buffer_capacity;
  _Atomic u32                buffer_front;
  _Atomic u32                buffer_back;
  _Atomic u32                buffer_count;
} cdrom_async_reader_t;

/* Lifecycle. */
void cdrom_async_reader_init(cdrom_async_reader_t* r);
void cdrom_async_reader_destroy(cdrom_async_reader_t* r);

/* Inline accessors.  Safe to call from the consumer thread; they read the
 * front-buffer slot referenced by the latest atomic m_buffer_front.  Caller
 * must have already
 * cdrom_async_reader_wait_for_read_to_complete()d before reading. */
cd_image_lba_t                     cdrom_async_reader_get_last_read_sector(const cdrom_async_reader_t* r);
const cdrom_async_sector_buffer_t* cdrom_async_reader_get_sector_buffer(const cdrom_async_reader_t* r);
const cd_image_subq_t*             cdrom_async_reader_get_sector_subq(const cdrom_async_reader_t* r);
u32                                cdrom_async_reader_get_buffered_sector_count(const cdrom_async_reader_t* r);
bool                               cdrom_async_reader_has_buffered_sectors(const cdrom_async_reader_t* r);
u32                                cdrom_async_reader_get_readahead_count(const cdrom_async_reader_t* r);

bool              cdrom_async_reader_has_media(const cdrom_async_reader_t* r);
cd_image_t*       cdrom_async_reader_get_media(cdrom_async_reader_t* r);
const cd_image_t* cdrom_async_reader_get_media_const(const cdrom_async_reader_t* r);
const char*       cdrom_async_reader_get_media_path(const cdrom_async_reader_t* r);

bool cdrom_async_reader_is_using_thread(const cdrom_async_reader_t* r);

/* Spawns the worker (joining any prior one).  readahead_count must be > 0; */
void cdrom_async_reader_start_thread(cdrom_async_reader_t* r, u32 readahead_count);

void cdrom_async_reader_stop_thread(cdrom_async_reader_t* r);

/* Hands ownership of `media` to the reader.  Pass NULL to clear.  The reader
 * frees the prior image (if any) via cd_image_destroy. */
void cdrom_async_reader_set_media(cdrom_async_reader_t* r, cd_image_t* media);

/* Detaches the current image and returns it to the caller; the caller is
 * then responsible for cd_image_destroy. */
cd_image_t* cdrom_async_reader_remove_media(cdrom_async_reader_t* r);

/* Precaches the entire image into memory if the underlying image type
 * supports it.  callback may be NULL. */
bool cdrom_async_reader_precache(cdrom_async_reader_t* r, progress_callback_t* callback, Error* error);

/* Asks the worker to read the given LBA.  If running in non-threaded mode
 * (StartThread never called), reads synchronously. */
void cdrom_async_reader_queue_read_sector(cdrom_async_reader_t* r, cd_image_lba_t lba);

/* Blocks until the most recently queued sector read completes; returns the
 * read result.  Cheap (no syscall) when buffer is already populated. */
bool cdrom_async_reader_wait_for_read_to_complete(cdrom_async_reader_t* r);

/* Blocks until any in-flight reads finish AND there are no pending seeks. */
void cdrom_async_reader_wait_for_idle(cdrom_async_reader_t* r);

/* One-off uncached read.  Used by the CDC for SubQ updates that don't move
 * the readahead position. */
bool cdrom_async_reader_read_sector_uncached(cdrom_async_reader_t* r, cd_image_lba_t lba,
                                             cd_image_subq_t* subq,
                                             cdrom_async_sector_buffer_t* data);

#endif /* CUPID_CORE_CDROM_ASYNC_READER_H */
