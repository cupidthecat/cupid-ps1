/*
 * cd_image_m3u: parses .m3u text playlists referencing one or more disc
 * images (cue/bin/img/iso) and presents them as a single cd_image_t with
 * the sub-image vtable methods wired up.  Each line in the .m3u is one
 * entry; '#'-prefixed lines and blank lines are ignored.  Relative paths
 * resolve against the m3u file's directory.
 */

#ifndef CUPID_UTIL_CD_IMAGE_M3U_H
#define CUPID_UTIL_CD_IMAGE_M3U_H

#include "cd_image.h"

/* Defined inline in cd_image.h via cd_image_open_m3u(); this header exists
 * for callers that want to be explicit about the dependency. */

#endif /* CUPID_UTIL_CD_IMAGE_M3U_H */
