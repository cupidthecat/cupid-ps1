/*
 * BufferStorage, MapAndSync, MapAndOrphan, BufferData.  BufferSubData
 * dropped.  Map/Unmap dispatch via fn pointers in the struct.
 */

#include "opengl_stream_buffer.h"

#include "common/align.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(GPUDevice);

void opengl_stream_buffer_bind  (opengl_stream_buffer_t* sb) { glBindBuffer(sb->target, sb->buffer_id); }
void opengl_stream_buffer_unbind(opengl_stream_buffer_t* sb) { glBindBuffer(sb->target, 0); }

static opengl_stream_buffer_mapping_t buffer_data_map(opengl_stream_buffer_t* sb, u32 alignment, u32 min_size)
{
  (void)min_size;
  opengl_stream_buffer_mapping_t m = { sb->cpu_buffer, 0, 0, sb->size / alignment };
  return m;
}

static u32 buffer_data_unmap(opengl_stream_buffer_t* sb, u32 used_size)
{
  if (used_size == 0) return 0;
  glBindBuffer(sb->target, sb->buffer_id);
  glBufferData(sb->target, used_size, sb->cpu_buffer, GL_STREAM_DRAW);
  return 0;
}

static u32 buffer_data_chunk(const opengl_stream_buffer_t* sb) { return sb->size; }

static void buffer_data_dtor(opengl_stream_buffer_t* sb)
{
  AlignedFree(sb->cpu_buffer);
}

static bool buffer_data_init(opengl_stream_buffer_t* sb, u32 size, Error* err)
{
  glGetError();
  glGenBuffers(1, &sb->buffer_id);
  glBindBuffer(sb->target, sb->buffer_id);
  glBufferData(sb->target, size, NULL, GL_STREAM_DRAW);
  const GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    Error_set_string_fmt(err, "Failed to create buffer: 0x%X", (unsigned)e);
    glBindBuffer(sb->target, 0);
    glDeleteBuffers(1, &sb->buffer_id);
    return false;
  }
  sb->cpu_buffer = (u8*)AlignedMalloc(size, 32);
  if (!sb->cpu_buffer) Panic("Failed to allocate CPU storage for GL buffer");
  sb->kind     = OPENGL_STREAM_BUFFER_KIND_BUFFER_DATA;
  sb->map_fn   = buffer_data_map;
  sb->unmap_fn = buffer_data_unmap;
  sb->chunk_fn = buffer_data_chunk;
  sb->dtor_fn  = buffer_data_dtor;
  return true;
}

static opengl_stream_buffer_mapping_t map_and_orphan_map(opengl_stream_buffer_t* sb, u32 alignment, u32 min_size)
{
  opengl_stream_buffer_bind(sb);

  if (sb->position > 0)
    sb->position = AlignUp(sb->position, alignment);

  if ((sb->position + min_size) > sb->size)
  {
    sb->position = 0;
    glBufferData(sb->target, sb->size, NULL, GL_STREAM_DRAW);
  }

  void* p = glMapBufferRange(sb->target, sb->position, sb->size - sb->position,
                             GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  Assert(p != NULL);

  opengl_stream_buffer_mapping_t m = { p, sb->position, sb->position / alignment,
                                       (sb->size - sb->position) / alignment };
  return m;
}

static u32 map_and_orphan_unmap(opengl_stream_buffer_t* sb, u32 used_size)
{
  DebugAssert((sb->position + used_size) <= sb->size);
  opengl_stream_buffer_bind(sb);
  if (used_size > 0)
    glFlushMappedBufferRange(sb->target, 0, used_size);
  glUnmapBuffer(sb->target);
  const u32 prev = sb->position;
  sb->position += used_size;
  return prev;
}

static u32 map_and_orphan_chunk(const opengl_stream_buffer_t* sb) { return sb->size; }
static void map_and_orphan_dtor (opengl_stream_buffer_t* sb)      { (void)sb; }

static bool map_and_orphan_init(opengl_stream_buffer_t* sb, u32 size, Error* err)
{
  glGetError();
  glGenBuffers(1, &sb->buffer_id);
  glBindBuffer(sb->target, sb->buffer_id);
  glBufferData(sb->target, size, NULL, GL_STREAM_DRAW);
  const GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    Error_set_string_fmt(err, "Failed to create buffer: 0x%X", (unsigned)e);
    glBindBuffer(sb->target, 0);
    glDeleteBuffers(1, &sb->buffer_id);
    return false;
  }
  sb->kind     = OPENGL_STREAM_BUFFER_KIND_MAP_AND_ORPHAN;
  sb->map_fn   = map_and_orphan_map;
  sb->unmap_fn = map_and_orphan_unmap;
  sb->chunk_fn = map_and_orphan_chunk;
  sb->dtor_fn  = map_and_orphan_dtor;
  return true;
}

static u32 sync_index_for_offset(const opengl_stream_buffer_t* sb, u32 offset)
{
  return offset / sb->bytes_per_block;
}

static void add_syncs_for_offset(opengl_stream_buffer_t* sb, u32 offset)
{
  const u32 end = sync_index_for_offset(sb, offset);
  for (; sb->used_block_index < end; ++sb->used_block_index)
  {
    DebugAssert(!sb->sync_objects[sb->used_block_index]);
    sb->sync_objects[sb->used_block_index] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  }
}

static void wait_for_sync(GLsync* sync)
{
  glClientWaitSync(*sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
  glDeleteSync(*sync);
  *sync = NULL;
}

static void ensure_syncs_waited_for_offset(opengl_stream_buffer_t* sb, u32 offset)
{
  u32 end = sync_index_for_offset(sb, offset) + 1;
  if (end > OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS) end = OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  for (; sb->available_block_index < end; ++sb->available_block_index)
  {
    DebugAssert(sb->sync_objects[sb->available_block_index]);
    wait_for_sync(&sb->sync_objects[sb->available_block_index]);
  }
}

static void allocate_space(opengl_stream_buffer_t* sb, u32 size)
{
  add_syncs_for_offset(sb, sb->position);
  ensure_syncs_waited_for_offset(sb, sb->position + size);

  if ((sb->position + size) > sb->size)
  {
    add_syncs_for_offset(sb, sb->size);
    sb->position = 0;
    wait_for_sync(&sb->sync_objects[0]);
    sb->available_block_index = 1;
    ensure_syncs_waited_for_offset(sb, size);
    sb->used_block_index = 0;
  }
}

static u32 sync_chunk(const opengl_stream_buffer_t* sb) { return sb->size / OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS; }

static void sync_dtor_common(opengl_stream_buffer_t* sb)
{
  for (u32 i = sb->available_block_index; i <= sb->used_block_index; ++i)
  {
    if (sb->sync_objects[i])
    {
      glDeleteSync(sb->sync_objects[i]);
      sb->sync_objects[i] = NULL;
    }
  }
}

static opengl_stream_buffer_mapping_t map_and_sync_map(opengl_stream_buffer_t* sb, u32 alignment, u32 min_size)
{
  if (sb->position > 0)
    sb->position = AlignUp(sb->position, alignment);
  allocate_space(sb, min_size);
  DebugAssert((sb->position + min_size) <= (sb->available_block_index * sb->bytes_per_block));

  opengl_stream_buffer_bind(sb);
  const u32 space = (sb->available_block_index * sb->bytes_per_block) - sb->position;
  void* p = glMapBufferRange(sb->target, sb->position, space,
                             GL_MAP_WRITE_BIT | GL_MAP_FLUSH_EXPLICIT_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  Assert(p != NULL);
  opengl_stream_buffer_mapping_t m = { p, sb->position, sb->position / alignment, space / alignment };
  return m;
}

static u32 map_and_sync_unmap(opengl_stream_buffer_t* sb, u32 used_size)
{
  DebugAssert((sb->position + used_size) <= sb->size);
  opengl_stream_buffer_bind(sb);
  if (used_size > 0) glFlushMappedBufferRange(sb->target, 0, used_size);
  glUnmapBuffer(sb->target);
  const u32 prev = sb->position;
  sb->position += used_size;
  return prev;
}

static void map_and_sync_dtor(opengl_stream_buffer_t* sb) { sync_dtor_common(sb); }

static bool map_and_sync_init(opengl_stream_buffer_t* sb, u32 size, Error* err)
{
  glGetError();
  glGenBuffers(1, &sb->buffer_id);
  glBindBuffer(sb->target, sb->buffer_id);
  glBufferData(sb->target, size, NULL, GL_STREAM_DRAW);
  const GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    Error_set_string_fmt(err, "Failed to create buffer: 0x%X", (unsigned)e);
    glBindBuffer(sb->target, 0);
    glDeleteBuffers(1, &sb->buffer_id);
    return false;
  }
  sb->bytes_per_block       = (size + OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS - 1) / OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  sb->available_block_index = OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  sb->kind     = OPENGL_STREAM_BUFFER_KIND_MAP_AND_SYNC;
  sb->map_fn   = map_and_sync_map;
  sb->unmap_fn = map_and_sync_unmap;
  sb->chunk_fn = sync_chunk;
  sb->dtor_fn  = map_and_sync_dtor;
  return true;
}

static opengl_stream_buffer_mapping_t buffer_storage_map(opengl_stream_buffer_t* sb, u32 alignment, u32 min_size)
{
  if (sb->position > 0)
    sb->position = AlignUp(sb->position, alignment);
  allocate_space(sb, min_size);
  DebugAssert((sb->position + min_size) <= (sb->available_block_index * sb->bytes_per_block));

  const u32 space = (sb->available_block_index * sb->bytes_per_block) - sb->position;
  opengl_stream_buffer_mapping_t m = { sb->mapped_ptr + sb->position, sb->position,
                                       sb->position / alignment, space / alignment };
  return m;
}

static u32 buffer_storage_unmap(opengl_stream_buffer_t* sb, u32 used_size)
{
  DebugAssert((sb->position + used_size) <= sb->size);
  if (!sb->coherent)
  {
    if (GLAD_GL_VERSION_4_5 || GLAD_GL_ARB_direct_state_access)
      glFlushMappedNamedBufferRange(sb->buffer_id, sb->position, used_size);
    else
    {
      opengl_stream_buffer_bind(sb);
      glFlushMappedBufferRange(sb->target, sb->position, used_size);
    }
  }
  const u32 prev = sb->position;
  sb->position += used_size;
  return prev;
}

static void buffer_storage_dtor(opengl_stream_buffer_t* sb)
{
  glBindBuffer(sb->target, sb->buffer_id);
  glUnmapBuffer(sb->target);
  glBindBuffer(sb->target, 0);
  sync_dtor_common(sb);
}

static bool buffer_storage_init(opengl_stream_buffer_t* sb, u32 size, Error* err, bool coherent)
{
  glGetError();
  glGenBuffers(1, &sb->buffer_id);
  glBindBuffer(sb->target, sb->buffer_id);

  const u32 flags     = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? GL_MAP_COHERENT_BIT : 0);
  const u32 map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | (coherent ? 0 : GL_MAP_FLUSH_EXPLICIT_BIT);

  if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage)
    glBufferStorage(sb->target, size, NULL, flags);
  else if (GLAD_GL_EXT_buffer_storage)
    glBufferStorageEXT(sb->target, size, NULL, flags);

  const GLenum e = glGetError();
  if (e != GL_NO_ERROR)
  {
    Error_set_string_fmt(err, "Failed to create buffer: 0x%X", (unsigned)e);
    glBindBuffer(sb->target, 0);
    glDeleteBuffers(1, &sb->buffer_id);
    return false;
  }

  sb->mapped_ptr = (u8*)glMapBufferRange(sb->target, 0, size, map_flags);
  AssertMsg(sb->mapped_ptr, "Persistent buffer was mapped");
  sb->coherent              = coherent;
  sb->bytes_per_block       = (size + OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS - 1) / OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  sb->available_block_index = OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  sb->kind     = OPENGL_STREAM_BUFFER_KIND_BUFFER_STORAGE;
  sb->map_fn   = buffer_storage_map;
  sb->unmap_fn = buffer_storage_unmap;
  sb->chunk_fn = sync_chunk;
  sb->dtor_fn  = buffer_storage_dtor;
  return true;
}

static opengl_stream_buffer_t* alloc_sb(GLenum target, u32 size)
{
  opengl_stream_buffer_t* sb = (opengl_stream_buffer_t*)calloc(1, sizeof(*sb));
  if (sb)
  {
    sb->target            = target;
    sb->size              = size;
    sb->available_block_index = OPENGL_STREAM_BUFFER_NUM_SYNC_POINTS;
  }
  return sb;
}

opengl_stream_buffer_t* opengl_stream_buffer_create(GLenum target, u32 size, Error* err)
{
  /* In terms of speed: persistent mapping > map+sync > map+orphan > bufferdata. */
  opengl_stream_buffer_t* sb = NULL;

  if (GLAD_GL_VERSION_4_4 || GLAD_GL_ARB_buffer_storage || GLAD_GL_EXT_buffer_storage)
  {
    sb = alloc_sb(target, size);
    if (sb && buffer_storage_init(sb, size, err, true))
    {
      DEV_LOG("Using BufferStorageStreamBuffer for %u byte 0x%X buffer.", size, (unsigned)target);
      return sb;
    }
    free(sb); sb = NULL;
  }

  if (GLAD_GL_VERSION_3_2 || GLAD_GL_ARB_sync || GLAD_GL_ES_VERSION_3_0)
  {
    sb = alloc_sb(target, size);
    if (sb && map_and_sync_init(sb, size, err))
    {
      DEV_LOG("Using MapAndSyncStreamBuffer for %u byte 0x%X buffer.", size, (unsigned)target);
      return sb;
    }
    free(sb); sb = NULL;
  }

  if (GLAD_GL_VERSION_3_0)
  {
    sb = alloc_sb(target, size);
    if (sb && map_and_orphan_init(sb, size, err))
    {
      DEV_LOG("Using MapAndOrphanStreamBuffer for %u byte 0x%X buffer.", size, (unsigned)target);
      return sb;
    }
    free(sb); sb = NULL;
  }

  sb = alloc_sb(target, size);
  if (sb && buffer_data_init(sb, size, err))
  {
    DEV_LOG("Using BufferDataStreamBuffer for %u byte 0x%X buffer.", size, (unsigned)target);
    return sb;
  }
  free(sb);
  return NULL;
}

void opengl_stream_buffer_destroy(opengl_stream_buffer_t* sb)
{
  if (!sb) return;
  if (sb->dtor_fn) sb->dtor_fn(sb);
  if (sb->buffer_id) glDeleteBuffers(1, &sb->buffer_id);
  free(sb);
}
