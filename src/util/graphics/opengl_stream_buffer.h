/*
 * `OpenGLStreamBuffer` virtual base + 5 derived classes, this is one struct
 * with a kind enum + per-kind state.  Map/Unmap go through fn pointers in the
 * struct (the hot dispatch path).
 *
 * Strategy ranking (descending speed): BufferStorage > MapAndSync >
 * MapAndOrphan > BufferData > BufferSubData (the latter not ported).
 */

#ifndef CUPID_UTIL_OPENGL_STREAM_BUFFER_H
#define CUPID_UTIL_OPENGL_STREAM_BUFFER_H

#include "common/types.h"

#include "glad/gl.h"

typedef struct Error Error;

typedef enum {
  OPENGL_STREAM_BUFFER_KIND_BUFFER_DATA = 0,
  OPENGL_STREAM_BUFFER_KIND_MAP_AND_ORPHAN,
  OPENGL_STREAM_BUFFER_KIND_MAP_AND_SYNC,
  OPENGL_STREAM_BUFFER_KIND_BUFFER_STORAGE,
} opengl_stream_buffer_kind_t;

typedef struct {
  void* pointer;
  u32   buffer_offset;
  u32   index_aligned;   /* offset / alignment, suitable for base vertex */
  u32   space_aligned;   /* remaining space / alignment */
} opengl_stream_buffer_mapping_t;

#define OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS 16

typedef struct opengl_stream_buffer opengl_stream_buffer_t;

typedef opengl_stream_buffer_mapping_t (*opengl_stream_buffer_map_fn)(opengl_stream_buffer_t* self,
                                                                     u32 alignment, u32 min_size);
typedef u32  (*opengl_stream_buffer_unmap_fn)(opengl_stream_buffer_t* self, u32 used_size);
typedef u32  (*opengl_stream_buffer_chunk_fn)(const opengl_stream_buffer_t* self);
typedef void (*opengl_stream_buffer_dtor_fn)(opengl_stream_buffer_t* self);

struct opengl_stream_buffer {
  GLenum target;
  GLuint buffer_id;
  u32    size;

  opengl_stream_buffer_kind_t kind;
  opengl_stream_buffer_map_fn   map_fn;
  opengl_stream_buffer_unmap_fn unmap_fn;
  opengl_stream_buffer_chunk_fn chunk_fn;
  opengl_stream_buffer_dtor_fn  dtor_fn;

  /* Position used by all map/sync/storage strategies. */
  u32 position;

  /* CPU-side scratch (BufferData / BufferSubData strategy). */
  u8* cpu_buffer;

  /* Persistent mapping (BufferStorage strategy). */
  u8*  mapped_ptr;
  bool coherent;

  /* Sync state (MapAndSync, BufferStorage). */
  u32    used_block_index;
  u32    available_block_index;
  u32    bytes_per_block;
  GLsync sync_objects[OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS];
};

/* Picks the best available strategy for the current GL context.  Returns
 * owning ptr; caller frees with opengl_stream_buffer_destroy(). */
opengl_stream_buffer_t* opengl_stream_buffer_create(GLenum target, u32 size, Error* err);
void                    opengl_stream_buffer_destroy(opengl_stream_buffer_t* sb);

ALWAYS_INLINE GLuint opengl_stream_buffer_get_id    (const opengl_stream_buffer_t* sb) { return sb->buffer_id; }
ALWAYS_INLINE GLenum opengl_stream_buffer_get_target(const opengl_stream_buffer_t* sb) { return sb->target;    }
ALWAYS_INLINE u32    opengl_stream_buffer_get_size  (const opengl_stream_buffer_t* sb) { return sb->size;      }

void opengl_stream_buffer_bind  (opengl_stream_buffer_t* sb);
void opengl_stream_buffer_unbind(opengl_stream_buffer_t* sb);

ALWAYS_INLINE opengl_stream_buffer_mapping_t opengl_stream_buffer_map(opengl_stream_buffer_t* sb, u32 align, u32 min_size)
{ return sb->map_fn(sb, align, min_size); }
ALWAYS_INLINE u32 opengl_stream_buffer_unmap(opengl_stream_buffer_t* sb, u32 used_size)
{ return sb->unmap_fn(sb, used_size); }
ALWAYS_INLINE u32 opengl_stream_buffer_chunk_size(const opengl_stream_buffer_t* sb)
{ return sb->chunk_fn(sb); }

#endif /* CUPID_UTIL_OPENGL_STREAM_BUFFER_H */
