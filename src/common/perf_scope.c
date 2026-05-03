/*
 *

 * behind compile flags; default build is no-op stubs.
 */
#include "common/perf_scope.h"
#include "common/string_util.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* #define ProfileWithPerf */
/* #define ProfileWithPerfJitDump */

#if defined(__linux__) && defined(ProfileWithPerf)

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

static FILE*           s_map_file = NULL;
static bool            s_map_file_opened = false;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

static void register_method(const void* ptr, size_t size, const char* symbol)
{
  pthread_mutex_lock(&s_mutex);

  if (!s_map_file) {
    if (s_map_file_opened) { pthread_mutex_unlock(&s_mutex); return; }
    char file[256];
    snprintf(file, sizeof(file), "/tmp/perf-%d.map", getpid());
    s_map_file = fopen(file, "wb");
    s_map_file_opened = true;
    if (!s_map_file) { pthread_mutex_unlock(&s_mutex); return; }
  }

  fprintf(s_map_file, "%" PRIx64 " %zx %s\n",
          (u64)(uintptr_t)ptr, size, symbol);
  fflush(s_map_file);

  pthread_mutex_unlock(&s_mutex);
}

#elif defined(__linux__) && defined(ProfileWithPerfJitDump)

#include <elf.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

enum {
  JIT_CODE_LOAD = 0,
  JIT_CODE_MOVE = 1,
  JIT_CODE_DEBUG_INFO = 2,
  JIT_CODE_CLOSE = 3,
  JIT_CODE_UNWINDING_INFO = 4
};

#pragma pack(push, 1)
typedef struct {
  u32 magic;        /* 'JiTD' = 0x4A695444 */
  u32 version;
  u32 header_size;
  u32 elf_mach;
  u32 pad1;
  u32 pid;
  u64 timestamp;
  u64 flags;
} jitdump_header_t;

typedef struct {
  u32 id;
  u32 total_size;
  u64 timestamp;
} jitdump_record_header_t;

typedef struct {
  jitdump_record_header_t header;
  u32 pid;
  u32 tid;
  u64 vma;
  u64 code_addr;
  u64 code_size;
  u64 code_index;
  /* trailing: name + code bytes */
} jitdump_code_load_t;
#pragma pack(pop)

static u64 jitdump_timestamp(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec;
}

static FILE*           s_jitdump_file = NULL;
static bool            s_jitdump_file_opened = false;
static pthread_mutex_t s_jitdump_mutex = PTHREAD_MUTEX_INITIALIZER;
static u32             s_jitdump_record_id;

static void register_method(const void* ptr, size_t size, const char* symbol)
{
  const u32 namelen = (u32)strlen(symbol) + 1u;

  pthread_mutex_lock(&s_jitdump_mutex);

  if (!s_jitdump_file) {
    if (!s_jitdump_file_opened) {
      char file[256];
      snprintf(file, sizeof(file), "jit-%d.dump", getpid());
      s_jitdump_file = fopen(file, "w+b");
      s_jitdump_file_opened = true;
      if (!s_jitdump_file) { pthread_mutex_unlock(&s_jitdump_mutex); return; }
    }

    void* perf_marker = mmap(NULL, 4096, PROT_READ | PROT_EXEC,
                             MAP_PRIVATE, fileno(s_jitdump_file), 0);
    (void)perf_marker;

    jitdump_header_t jh = {0};
    jh.magic = 0x4A695444u; /* JiTD */
    jh.version = 1u;
    jh.header_size = (u32)sizeof(jh);
#if defined(__aarch64__)
    jh.elf_mach = EM_AARCH64;
#else
    jh.elf_mach = EM_X86_64;
#endif
    jh.pid = (u32)getpid();
    jh.timestamp = jitdump_timestamp();
    fwrite(&jh, sizeof(jh), 1, s_jitdump_file);
  }

  jitdump_code_load_t cl = {0};
  cl.header.id = JIT_CODE_LOAD;
  cl.header.total_size = (u32)sizeof(cl) + namelen + (u32)size;
  cl.header.timestamp = jitdump_timestamp();
  cl.pid = (u32)getpid();
  cl.tid = (u32)syscall(SYS_gettid);
  cl.vma = 0;
  cl.code_addr = (u64)(uintptr_t)ptr;
  cl.code_size = (u64)size;
  cl.code_index = s_jitdump_record_id++;
  fwrite(&cl, sizeof(cl), 1, s_jitdump_file);
  fwrite(symbol, namelen, 1, s_jitdump_file);
  fwrite(ptr, size, 1, s_jitdump_file);
  fflush(s_jitdump_file);

  pthread_mutex_unlock(&s_jitdump_mutex);
}

#endif /* perf integration variants */

#if defined(__linux__) && (defined(ProfileWithPerf) || defined(ProfileWithPerfJitDump))

void perf_scope_register(const perf_scope_t* ps, const void* ptr, size_t size, const char* symbol)
{
  char full_symbol[128];
  if (perf_scope_has_prefix(ps))
    snprintf(full_symbol, sizeof(full_symbol), "%s_%s", ps->prefix, symbol);
  else
    string_util_strlcpy_cstr(full_symbol, symbol, sizeof(full_symbol));
  register_method(ptr, size, full_symbol);
}

void perf_scope_register_pc(const perf_scope_t* ps, const void* ptr, size_t size, u32 pc)
{
  char full_symbol[128];
  if (perf_scope_has_prefix(ps))
    snprintf(full_symbol, sizeof(full_symbol), "%s_%08X", ps->prefix, pc);
  else
    snprintf(full_symbol, sizeof(full_symbol), "%08X", pc);
  register_method(ptr, size, full_symbol);
}

void perf_scope_register_key(const perf_scope_t* ps, const void* ptr, size_t size, const char* prefix, u64 key)
{
  char full_symbol[128];
  if (perf_scope_has_prefix(ps))
    snprintf(full_symbol, sizeof(full_symbol), "%s_%s%016" PRIX64, ps->prefix, prefix, key);
  else
    snprintf(full_symbol, sizeof(full_symbol), "%s%016" PRIX64, prefix, key);
  register_method(ptr, size, full_symbol);
}

#else

void perf_scope_register(const perf_scope_t* ps, const void* ptr, size_t size, const char* symbol)
{
  (void)ps; (void)ptr; (void)size; (void)symbol;
}

void perf_scope_register_pc(const perf_scope_t* ps, const void* ptr, size_t size, u32 pc)
{
  (void)ps; (void)ptr; (void)size; (void)pc;
}

void perf_scope_register_key(const perf_scope_t* ps, const void* ptr, size_t size, const char* prefix, u64 key)
{
  (void)ps; (void)ptr; (void)size; (void)prefix; (void)key;
}

#endif
