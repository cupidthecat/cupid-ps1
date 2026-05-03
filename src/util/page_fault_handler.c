/*
 *

 * x64 / ARM32 / ARM64 / RISC-V64 / LoongArch64.
 */
#include "page_fault_handler.h"

#include "common/assert.h"
#include "common/crash_handler.h"
#include "common/error.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <ucontext.h>

#if defined(CPU_ARCH_ARM64)
static bool is_store_instruction(const void* ptr)
{
  u32 bits;
  memcpy(&bits, ptr, sizeof(bits));
  /* Mirrors VIXL Instruction::IsStore(). */
  if ((bits & 0x0a000000) != 0x08000000) return false;
  if ((bits & 0x3a000000) == 0x28000000) return (bits & (1u << 22)) == 0;
  switch (bits & 0xC4C00000) {
    case 0x00000000: case 0x40000000: case 0x80000000: case 0xC0000000:
    case 0x04000000: case 0x44000000: case 0x84000000: case 0xC4000000:
    case 0x04800000:
      return true;
    default:
      return false;
  }
}
#elif defined(CPU_ARCH_RISCV64)
static bool is_store_instruction(const void* ptr)
{
  u32 bits;
  memcpy(&bits, ptr, sizeof(bits));
  return (bits & 0x7Fu) == 0b0100011u;
}
#endif

static pthread_mutex_t          s_mu        = PTHREAD_MUTEX_INITIALIZER;
static bool                     s_in_handler;
static bool                     s_installed;
static page_fault_handler_cb_t  s_cb;

void page_fault_handler_set_callback(page_fault_handler_cb_t cb) { s_cb = cb; }

page_fault_handler_result_t page_fault_handler_handle(void* exception_pc, void* fault_address, bool is_write)
{
  if (s_cb) return s_cb(exception_pc, fault_address, is_write);
  return PAGE_FAULT_HANDLER_EXECUTE_NEXT;
}

static void signal_handler(int sig, siginfo_t* info, void* ctx)
{
  void* fault_addr = info->si_addr;
  void* pc         = NULL;
  bool  is_write   = false;

  ucontext_t* uc = (ucontext_t*)ctx;
#if defined(CPU_ARCH_X64)
  pc       = (void*)(uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
  is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(CPU_ARCH_X86)
  pc       = (void*)(uintptr_t)uc->uc_mcontext.gregs[REG_EIP];
  is_write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#elif defined(CPU_ARCH_ARM64)
  pc       = (void*)(uintptr_t)uc->uc_mcontext.pc;
  is_write = is_store_instruction(pc);
#elif defined(CPU_ARCH_ARM32)
  pc       = (void*)(uintptr_t)uc->uc_mcontext.arm_pc;
  is_write = (uc->uc_mcontext.error_code & (1u << 11)) != 0;
#elif defined(CPU_ARCH_RISCV64)
  pc       = (void*)(uintptr_t)uc->uc_mcontext.__gregs[0]; /* REG_PC */
  is_write = is_store_instruction(pc);
#endif

  pthread_mutex_lock(&s_mu);
  page_fault_handler_result_t result = PAGE_FAULT_HANDLER_EXECUTE_NEXT;
  if (!s_in_handler) {
    s_in_handler = true;
    result = page_fault_handler_handle(pc, fault_addr, is_write);
    s_in_handler = false;
  }
  pthread_mutex_unlock(&s_mu);

  if (result == PAGE_FAULT_HANDLER_CONTINUE_EXECUTION)
    return;

  /* Hand off to crash handler so we get a backtrace. */
  crash_handler_signal_handler(sig, info, ctx);
}

bool page_fault_handler_install(Error* err)
{
  pthread_mutex_lock(&s_mu);
  AssertMsg(!s_installed, "Page fault handler has already been installed.");

  struct sigaction sa = {0};
  sigemptyset(&sa.sa_mask);
  sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
  sa.sa_sigaction = signal_handler;

  bool ok = true;
  if (sigaction(SIGSEGV, &sa, NULL) != 0) {
    Error_set_errno_prefix(err, "sigaction(SIGSEGV) failed: ", errno);
    ok = false;
  }
#if defined(CPU_ARCH_ARM64)
  if (ok && sigaction(SIGBUS, &sa, NULL) != 0) {
    Error_set_errno_prefix(err, "sigaction(SIGBUS) failed: ", errno);
    ok = false;
  }
#endif
  if (ok) s_installed = true;
  pthread_mutex_unlock(&s_mu);
  return ok;
}
