#include "fmv_dump.h"

#ifdef CUPID_FMV_DUMP_BUILD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMV_MAX_STAGES 32u
#define FMV_FRAME_LIMIT 3u
#define FMV_AUDIO_LIMIT 20u   /* sectors */

static struct {
  const char* env_path;
  int  env_checked;
  int  enabled;
  int  armed_video;
  int  armed_audio;
  u32  frame;
  u32  audio_count;     /* increments per A1 fire (one per XA sector) */
  struct { char name[16]; FILE* fp; } stages[FMV_MAX_STAGES];
  u32  num_stages;
} s_fmv;

static void fmv_check_env(void)
{
  if (s_fmv.env_checked) return;
  s_fmv.env_checked = 1;
  s_fmv.env_path = getenv("CUPID_FMV_DUMP");
  s_fmv.enabled  = (s_fmv.env_path != NULL && s_fmv.env_path[0] != '\0');
}

static FILE* fmv_open_stage(const char* stage)
{
  for (u32 i = 0; i < s_fmv.num_stages; i++)
    if (strcmp(s_fmv.stages[i].name, stage) == 0)
      return s_fmv.stages[i].fp;
  if (s_fmv.num_stages >= FMV_MAX_STAGES) return NULL;
  char path[1024];
  snprintf(path, sizeof(path), "%s.%s", s_fmv.env_path, stage);
  FILE* fp = fopen(path, "w");
  if (!fp) return NULL;
  snprintf(s_fmv.stages[s_fmv.num_stages].name,
           sizeof(s_fmv.stages[s_fmv.num_stages].name), "%s", stage);
  s_fmv.stages[s_fmv.num_stages].fp = fp;
  s_fmv.num_stages++;
  return fp;
}

void fmv_dump(const char* stage, const void* buf, u32 nbytes, u64 tag)
{
  fmv_check_env();
  if (!s_fmv.enabled) return;

  const int is_audio = (stage[0] == 'A');
  const int is_t1 = (stage[0] == 'T' && stage[1] == '1' && stage[2] == '\0');
  const int is_t2 = (stage[0] == 'T' && stage[1] == '2');
  const int is_a1 = (stage[0] == 'A' && stage[1] == '1' && stage[2] == '\0');

  /* Video arming: first completed RLE block. T1 always dumps so the
   * macroblock feed before first T2 is captured.
   * Audio arming: first XA sector ProcessXAADPCMSector entry. */
  if (!s_fmv.armed_video && is_t2) s_fmv.armed_video = 1;
  if (!s_fmv.armed_audio && is_a1) s_fmv.armed_audio = 1;

  if (is_audio) {
    if (!s_fmv.armed_audio) return;
    if (is_a1) s_fmv.audio_count++;
    if (s_fmv.audio_count > FMV_AUDIO_LIMIT) return;
  } else {
    if (!is_t1 && !s_fmv.armed_video) return;
    if (s_fmv.armed_video && s_fmv.frame >= FMV_FRAME_LIMIT) return;
  }

  FILE* fp = fmv_open_stage(stage);
  if (!fp) return;
  const u32 ctr = is_audio ? s_fmv.audio_count : s_fmv.frame;
  fprintf(fp, "%s=%u tag=%016llx n=%u\n",
          is_audio ? "sec" : "frame",
          ctr, (unsigned long long)tag, nbytes);
  const u8* p = (const u8*)buf;
  for (u32 i = 0; i < nbytes; i++) {
    fprintf(fp, "%02x", p[i]);
    if ((i & 31u) == 31u) fputc('\n', fp);
  }
  if ((nbytes & 31u) != 0u) fputc('\n', fp);
  fflush(fp);
}

u32  fmv_dump_frame(void)     { return s_fmv.frame; }
void fmv_dump_inc_frame(void)
{
  fmv_check_env();
  if (!s_fmv.armed_video) return;
  s_fmv.frame++;
}

#endif /* CUPID_FMV_DUMP_BUILD */
