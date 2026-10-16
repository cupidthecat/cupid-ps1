#ifndef CUPID_CORE_FMV_DUMP_H
#define CUPID_CORE_FMV_DUMP_H

#include "common/types.h"

#ifdef CUPID_FMV_DUMP_BUILD
void fmv_dump(const char* stage, const void* buf, u32 nbytes, u64 tag);
u32  fmv_dump_frame(void);
void fmv_dump_inc_frame(void);
#define FMV_DUMP(stage, buf, n, tag) fmv_dump((stage), (buf), (n), (tag))
#define FMV_DUMP_INC_FRAME()         fmv_dump_inc_frame()
#define FMV_DUMP_FRAME()             fmv_dump_frame()
#else
#define FMV_DUMP(stage, buf, n, tag) \
  ((void)(stage), (void)(buf), (void)(n), (void)(tag))
#define FMV_DUMP_INC_FRAME()         ((void)0)
#define FMV_DUMP_FRAME()             (0u)
#endif

#endif
