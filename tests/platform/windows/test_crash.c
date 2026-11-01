/* cupid-ps1 unhandled exception reporting and cleanup in an isolated child. */
#include "common/crash_handler.h"
#include "common/windows/windows_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s (%lu)\n", __FILE__, __LINE__, #x, (unsigned long)GetLastError()); exit(1); } } while (0)

static void cleanup(void)
{
  fputs("cleanup-called-once\n", stderr); fflush(stderr);
}
static LONG WINAPI previous_filter(PEXCEPTION_POINTERS pointers)
{
  (void)pointers;
  fputs("previous-filter-called\n", stderr); fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char** argv)
{
  if (argc == 2 && !strcmp(argv[1], "--child")) {
    SetUnhandledExceptionFilter(previous_filter);
    CHECK(crash_handler_install(cleanup));
    CHECK(crash_handler_install(cleanup)); /* Reinstallation must not chain to itself. */
    RaiseException(0xE0C011ABu, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 4;
  }
  wchar_t program[32768], command[32768];
  DWORD length = GetModuleFileNameW(NULL, program, 32768);
  CHECK(length && length + 12 < 32768);
  CHECK(swprintf(command, 32768, L"\"%ls\" --child", program) > 0);
  SECURITY_ATTRIBUTES security = {sizeof(security), NULL, TRUE};
  HANDLE read_pipe, write_pipe;
  CHECK(CreatePipe(&read_pipe, &write_pipe, &security, 0));
  CHECK(SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0));
  STARTUPINFOW startup = {0}; startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = write_pipe; startup.hStdError = write_pipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process = {0};
  CHECK(CreateProcessW(program, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
  CloseHandle(write_pipe); CloseHandle(process.hThread);
  CHECK(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0);
  DWORD code; CHECK(GetExitCodeProcess(process.hProcess, &code)); CloseHandle(process.hProcess);
  char output[16384]; DWORD used = 0, count;
  while (used < sizeof(output) - 1 && ReadFile(read_pipe, output + used, (DWORD)sizeof(output) - 1 - used, &count, NULL) && count)
    used += count;
  CloseHandle(read_pipe); output[used] = 0;
  if (code != 0xE0C011ABu) fprintf(stderr, "child=%08lX\n%s", (unsigned long)code, output);
  CHECK(code == 0xE0C011ABu);
  CHECK(strstr(output, "Windows exception E0C011AB"));
  const char* marker = strstr(output, "cleanup-called-once");
  CHECK(marker && !strstr(marker + 1, "cleanup-called-once"));
  CHECK(strstr(output, "previous-filter-called"));
  puts("Windows crash diagnostics, cleanup and previous-filter chaining passed");
  return 0;
}
