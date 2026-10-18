/* Run registered tests in isolated child processes and report every result. */
#include "tests/test_harness.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static test_entry_t* s_head  = NULL;
static test_entry_t* s_tail  = NULL;
static u32           s_count = 0;

void test_register(test_entry_t* entry)
{
  entry->next = NULL;
  if (s_tail)
    s_tail->next = entry;
  else
    s_head = entry;
  s_tail = entry;
  s_count++;
}

void test_fail(const char* file, int line, const char* format, ...)
{
  fprintf(stderr, "    %s:%d ", file, line);
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fputc('\n', stderr);
  fflush(stdout);
  fflush(stderr);
  _exit(2);
}

typedef struct {
  pid_t         pid;
  test_entry_t* entry;
  FILE*         output;
  char*         directory;
} worker_t;

static char* make_directory(const char* resources)
{
  const char* base = getenv("TMPDIR");
  if (!base || !*base) base = "/tmp";
  const size_t capacity  = strlen(base) + sizeof("/cupid-worker-XXXXXX");
  char*        directory = malloc(capacity);
  if (!directory) return NULL;
  snprintf(directory, capacity, "%s/cupid-worker-XXXXXX", base);
  if (!mkdtemp(directory)) {
    free(directory);
    return NULL;
  }
  if (resources) {
    const size_t link_capacity = strlen(directory) + sizeof("/data");
    char*        link          = malloc(link_capacity);
    if (!link) {
      rmdir(directory);
      free(directory);
      return NULL;
    }
    snprintf(link, link_capacity, "%s/data", directory);
    const int result = symlink(resources, link);
    free(link);
    if (result != 0) {
      rmdir(directory);
      free(directory);
      return NULL;
    }
  }
  return directory;
}

static void destroy_workers(worker_t* workers, u32 count)
{
  for (u32 i = 0; i < count; i++) {
    char* directory = workers[i].directory;
    if (!directory) continue;
    const size_t capacity = strlen(directory) + sizeof("/data");
    char*        link     = malloc(capacity);
    if (link) {
      snprintf(link, capacity, "%s/data", directory);
      unlink(link);
      free(link);
    }
    /* Keep files produced by tests available for inspection. */
    rmdir(directory);
    free(directory);
  }
  free(workers);
}

static FILE* open_output(void)
{
  const char* directory = getenv("TMPDIR");
  if (!directory || !*directory) directory = "/tmp";
  const size_t capacity = strlen(directory) + sizeof("/cupid-test-XXXXXX");
  char*        path     = malloc(capacity);
  if (!path) return NULL;
  snprintf(path, capacity, "%s/cupid-test-XXXXXX", directory);
  const int descriptor = mkstemp(path);
  if (descriptor < 0) {
    free(path);
    return NULL;
  }
  if (unlink(path) != 0) {
    close(descriptor);
    free(path);
    return NULL;
  }
  free(path);
  FILE* output = fdopen(descriptor, "w+");
  if (!output) close(descriptor);
  return output;
}

static bool start_worker(worker_t* worker, test_entry_t* entry)
{
  FILE* output = open_output();
  if (!output) {
    fprintf(stderr, "Could not create output file for %s.%s: %s\n", entry->suite, entry->name, strerror(errno));
    return false;
  }
  fflush(stdout);
  fflush(stderr);
  const pid_t pid = fork();
  if (pid < 0) {
    fprintf(stderr, "Could not fork %s.%s: %s\n", entry->suite, entry->name, strerror(errno));
    fclose(output);
    return false;
  }
  if (pid == 0) {
    const int descriptor = fileno(output);
    if (dup2(descriptor, STDOUT_FILENO) < 0 || dup2(descriptor, STDERR_FILENO) < 0) _exit(3);
    fclose(output);
    if (setvbuf(stdout, NULL, _IOLBF, 0) != 0) _exit(3);
    if (worker->directory && chdir(worker->directory) != 0) {
      fprintf(stderr, "Could not enter test directory: %s\n", strerror(errno));
      _exit(3);
    }
    entry->fn();
    const bool output_failed = fflush(stdout) != 0 || fflush(stderr) != 0 || ferror(stdout) || ferror(stderr);
    _exit(output_failed ? 3 : 0);
  }
  worker->pid    = pid;
  worker->entry  = entry;
  worker->output = output;
  return true;
}

static bool emit_output(FILE* output)
{
  if (fseek(output, 0, SEEK_SET) != 0) return false;
  char   buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), output)) != 0) {
    if (fwrite(buffer, 1, count, stdout) != count) return false;
  }
  return !ferror(output);
}

static void report_result(test_entry_t* entry, int result, u32* passed, u32* failed)
{
  if (result == 0) {
    printf("[  OK  ] %s.%s\n", entry->suite, entry->name);
    (*passed)++;
  } else if (result >= 128) {
    printf("[CRASH ] %s.%s (signal %d)\n", entry->suite, entry->name, result - 128);
    (*failed)++;
  } else {
    printf("[ FAIL ] %s.%s (exit %d)\n", entry->suite, entry->name, result);
    (*failed)++;
  }
  fflush(stdout);
}

static bool matches(const test_entry_t* entry, const char* filter)
{
  if (!filter) return true;
  char name[256];
  snprintf(name, sizeof(name), "%s.%s", entry->suite, entry->name);
  return strstr(name, filter) != NULL;
}

static void usage(const char* program, FILE* stream)
{
  fprintf(stream, "Usage: %s [--jobs N] [filter]\n", program);
}

int main(int argc, char** argv)
{
  const char* filter   = NULL;
  u32         jobs     = 1;
  bool        jobs_set = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--help") == 0) {
      usage(argv[0], stdout);
      return 0;
    }
    if (strcmp(argv[i], "--jobs") == 0) {
      if (jobs_set || ++i >= argc) {
        usage(argv[0], stderr);
        return 2;
      }
      char* end;
      errno                      = 0;
      const unsigned long parsed = strtoul(argv[i], &end, 10);
      if (errno || !*argv[i] || *end || parsed == 0 || parsed > UINT_MAX || argv[i][0] == '-') {
        usage(argv[0], stderr);
        return 2;
      }
      jobs     = (u32)parsed;
      jobs_set = true;
    } else if (filter || strncmp(argv[i], "--", 2) == 0) {
      usage(argv[0], stderr);
      return 2;
    } else {
      filter = argv[i];
    }
  }

  u32 selected = 0;
  for (test_entry_t* entry = s_head; entry; entry = entry->next)
    selected += matches(entry, filter);
  if (jobs > selected) jobs = selected;
  worker_t* workers = jobs ? calloc(jobs, sizeof(*workers)) : NULL;
  if (jobs && !workers) {
    fprintf(stderr, "Could not allocate test workers.\n");
    return 1;
  }
  if (jobs > 1) {
    char* resources = realpath("data", NULL);
    for (u32 slot = 0; slot < jobs; slot++) {
      workers[slot].directory = make_directory(resources);
      if (!workers[slot].directory) {
        fprintf(stderr, "Could not create worker directory: %s\n", strerror(errno));
        free(resources);
        destroy_workers(workers, jobs);
        return 1;
      }
    }
    free(resources);
  }

  u32           ran = 0, passed = 0, failed = 0, live = 0;
  test_entry_t* next = s_head;
  while (next || live) {
    for (u32 slot = 0; slot < jobs && next; slot++) {
      if (workers[slot].pid) continue;
      while (next && !matches(next, filter))
        next = next->next;
      if (!next) break;
      test_entry_t* entry = next;
      next                = next->next;
      ran++;
      if (start_worker(&workers[slot], entry)) {
        live++;
      } else {
        printf("[ RUN  ] %s.%s\n", entry->suite, entry->name);
        report_result(entry, -1, &passed, &failed);
      }
    }
    if (!live) {
      if (!jobs) break;
      continue;
    }
    int   status;
    pid_t pid;
    do {
      pid = waitpid(-1, &status, 0);
    } while (pid < 0 && errno == EINTR);
    if (pid < 0) {
      fprintf(stderr, "Could not wait for test workers: %s\n", strerror(errno));
      for (u32 slot = 0; slot < jobs; slot++) {
        if (!workers[slot].pid) continue;
        kill(workers[slot].pid, SIGTERM);
        while (waitpid(workers[slot].pid, NULL, 0) < 0 && errno == EINTR) {
        }
        fclose(workers[slot].output);
      }
      destroy_workers(workers, jobs);
      return 1;
    }
    for (u32 slot = 0; slot < jobs; slot++) {
      worker_t* worker = &workers[slot];
      if (worker->pid != pid) continue;
      printf("[ RUN  ] %s.%s\n", worker->entry->suite, worker->entry->name);
      int result = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
      if (!emit_output(worker->output)) {
        fprintf(stderr, "Could not read output for %s.%s.\n", worker->entry->suite, worker->entry->name);
        result = -1;
      }
      fclose(worker->output);
      report_result(worker->entry, result, &passed, &failed);
      worker->pid    = 0;
      worker->entry  = NULL;
      worker->output = NULL;
      live--;
      break;
    }
  }
  destroy_workers(workers, jobs);
  printf("\n%u test%s ran: %u passed, %u failed (registered: %u)\n", ran, ran == 1 ? "" : "s", passed, failed, s_count);
  if (ran == 0) fprintf(stderr, "No tests matched%s%s.\n", filter ? ": " : "", filter ? filter : "");
  return ran > 0 && ran == selected && failed == 0 ? 0 : 1;
}
