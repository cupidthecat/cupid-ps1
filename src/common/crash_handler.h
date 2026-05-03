/*
 * backtrace / dump-file logic is deferred until first useful consumer.
 * Every public function is implemented (no-ops where appropriate).
 */

#ifndef CUPID_COMMON_CRASH_HANDLER_H
#define CUPID_COMMON_CRASH_HANDLER_H

#include "types.h"

#include <signal.h>

typedef void (*crash_handler_cleanup_t)(void);

bool crash_handler_install(crash_handler_cleanup_t cleanup);
void crash_handler_set_write_directory(const char* dir, u32 dir_len);
void crash_handler_write_dump_for_caller(const char* msg, u32 msg_len);

void crash_handler_signal_handler(int signo, siginfo_t* si, void* ctx);

#endif /* CUPID_COMMON_CRASH_HANDLER_H */
