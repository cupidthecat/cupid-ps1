/*
 * PS1 CD-ROM controller emulation.  Tracks the SCEx mech state machine
 * (commands, drive position, sector pipeline), produces XA-ADPCM and CDDA
 * audio frames for the SPU, and drives interrupt requests to the CPU via
 * the interrupt controller.
 *
 * snake-case prefix.  All state is process-wide and lives as file-static
 * globals in cdrom.c (the PS1 has exactly one CD drive).
 *
 * std::tuple<s16,s16> GetAudioFrame() -> typed (s16*, s16*) out-params.
 * std::unique_ptr<CDImage> for media -> raw cd_image_t* whose ownership
 *   passes through cdrom_async_reader_t.
 */

#ifndef CUPID_CORE_CDROM_H
#define CUPID_CORE_CDROM_H

#include "core/types.h"
#include "util/cd_image.h"

typedef struct Error            Error;
typedef struct state_wrapper    state_wrapper_t;

void cdrom_initialize(void);
void cdrom_shutdown(void);
void cdrom_reset(void);
bool cdrom_do_state(state_wrapper_t* sw);

bool              cdrom_has_media(void);
const char*       cdrom_get_media_path(void);
u32               cdrom_get_current_sub_image(void);
const cd_image_t* cdrom_get_media(void);
disc_region_t     cdrom_get_disc_region(void);
bool              cdrom_is_media_ps1_disc(void);
bool              cdrom_is_media_audio_cd(void);
bool              cdrom_does_media_region_match_console(void);

/* Inserts the disc.  Ownership of `media` transfers to the CDROM module on
 * success; on failure the caller still owns it.  serial / title / save_title
 * can be empty (NULL or len==0); they're used only to look up sidecar SBI/LSD
 * subchannel files. */
bool cdrom_insert_media(cd_image_t* media, disc_region_t region,
                        const char* serial, size_t serial_len,
                        const char* title, size_t title_len,
                        const char* save_title, size_t save_title_len,
                        Error* error);

/* Removes the disc.  If for_disc_swap is true an extended spin-down delay is
 * applied before the next disc can be inserted (some games dislike fast
 * swaps).  Returns the previously-inserted image (caller takes ownership)
 * or NULL if nothing was inserted. */
cd_image_t* cdrom_remove_media(bool for_disc_swap);

/* Forces the underlying image into memory.  Returns false on failure. */
bool cdrom_precache_media(void);

/* Reports whether SBI/LSD overrides or non-default subchannel data are in
 * play; used by hashing/save-state code to refuse leaderboard runs etc. */
bool cdrom_has_non_standard_or_replacement_subq(void);

/* Called when the CPU clock changes (overclock toggle / region switch). */
void cdrom_cpu_clock_changed(void);

u8   cdrom_read_register(u32 offset);
void cdrom_write_register(u32 offset, u8 value);

void cdrom_dma_read(u32* words, u32 word_count);

void cdrom_set_readahead_sectors(u32 readahead_sectors);

/* Disables maximum-rate read speedup if currently active.  Called when MDEC
 * starts a frame because reading too fast can overrun the output buffers. */
void cdrom_disable_read_speedup(void);

/* Pops a frame from the CDDA/XA audio FIFO and writes the (left, right)
 * sample pair after the CD-side volume matrix has been applied.  Returns
 * (0,0) when the FIFO is empty. */
void cdrom_get_audio_frame(s16* out_left, s16* out_right);

u32 cdrom_dbg_drive_state(void);
u32 cdrom_dbg_current_lba(void);
u32 cdrom_dbg_requested_lba(void);
u32 cdrom_dbg_buffer_status(void);
u32 cdrom_dbg_command_state(void);

#include <stdio.h>
void cdrom_dbg_dump_recent_commands(FILE* out);

#endif /* CUPID_CORE_CDROM_H */
