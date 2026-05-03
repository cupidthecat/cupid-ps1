/*
 * BIOS image loader + hash database + fast-boot patcher.  The hash table
 * for human readability, not lookup speed (we linear-scan; <100 entries).
 *
 * External (parallel-agent) dependencies:
 *   core_get_string_setting_value(section, key, def, out) ; settings agent
 *   emu_folders_get_bios()                                ; folders agent
 *   settings_get_console_region_name(region)              ; settings agent
 *
 * Stubs for the above can be supplied by the calling translation unit; until
 * the settings/folders modules land, bios_get_image() simply returns false.
 */

#include "core/bios.h"

#include "common/assert.h"
#include "common/error.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/md5_digest.h"
#include "common/path.h"
#include "common/small_string.h"
#include "common/string_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LOG_CHANNEL(BIOS);

extern const char* emu_folders_get_bios(void) __attribute__((weak));
extern void core_get_string_setting_value(const char* section, const char* key,
                                          const char* def, small_string_t* out)
  __attribute__((weak));
extern const char* settings_get_console_region_name(console_region_t region) __attribute__((weak));

static const char* default_console_region_name(console_region_t region)
{
  switch (region)
  {
    case CONSOLE_REGION_NTSC_J: return "NTSC-J";
    case CONSOLE_REGION_NTSC_U: return "NTSC-U";
    case CONSOLE_REGION_PAL:    return "PAL";
    case CONSOLE_REGION_AUTO:
    default:                    return "Auto";
  }
}

static const char* region_name(console_region_t r)
{
  return settings_get_console_region_name ? settings_get_console_region_name(r)
                                          : default_console_region_name(r);
}

 /* Build a heap-allocated bios_hash_t from a 32-char hex string at compile-
 * call time. */
static void bios_make_hash_from_string(bios_hash_t out, const char* hex)
{
  string_util_parse_fixed_hex_string(hex, out, BIOS_HASH_SIZE);
}

static bios_image_info_t s_image_info_by_hash[] = {
  {"SCPH-1000, DTL-H1000 (v1.0)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 50, {0}},
  {"SCPH-1001, 5003, DTL-H1201, H3001 (v2.2 12-04-95 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-1002, DTL-H1002 (v2.0 05-10-95 E)", CONSOLE_REGION_PAL, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-1002, DTL-H1102 (v2.1 07-17-95 E)", CONSOLE_REGION_PAL, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-1002, DTL-H1202, H3002 (v2.2 12-04-95 E)", CONSOLE_REGION_PAL, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"DTL-H1100 (v2.2 03-06-96 D)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 20, {0}},
  {"SCPH-3000, DTL-H1000H (v1.1 01-22-95)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-1001, DTL-H1001 (v2.0 05-07-95 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-3500 (v2.1 07-17-95 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-1001, DTL-H1101 (v2.1 07-17-95 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-5000, DTL-H1200, H3000 (v2.2 12-04-95 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 5, {0}},
  {"SCPH-5500 (v3.0 09-09-96 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 5, {0}},
  {"SCPH-5501, 5503, 7003 (v3.0 11-18-96 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 5, {0}},
  {"SCPH-5502, 5552 (v3.0 01-06-97 E)", CONSOLE_REGION_PAL, false, BIOS_FAST_BOOT_PATCH_TYPE1, 5, {0}},
  {"SCPH-7000, 7500, 9000 (v4.0 08-18-97 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-7000W (v4.1 11-14-97 A)", CONSOLE_REGION_NTSC_J, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-7001, 7501, 7503, 9001, 9003, 9903 (v4.1 12-16-97 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-7002, 7502, 9002 (v4.1 12-16-97 E)", CONSOLE_REGION_PAL, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-100 (v4.3 03-11-00 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-101 (v4.4 03-24-00 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-101 (v4.5 05-25-00 A)", CONSOLE_REGION_NTSC_U, false, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"SCPH-102 (v4.4 03-24-00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE1, 20, {0}},
  {"SCPH-102 (v4.5 05-25-00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE1, 20, {0}},
  {"SCPH-1000R (v4.5 05-25-00 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE1, 10, {0}},
  {"PS2, SCPH-18000 (v5.0 10-27-00 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30003 (v5.0 09-02-00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, DTL-H10000 (v5.0 01/17/00 T)", CONSOLE_REGION_AUTO, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-10000 (v5.0 01/17/00 T)", CONSOLE_REGION_AUTO, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H10000 (v5.0 02/17/00 T)", CONSOLE_REGION_AUTO, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-10000/SCPH-15000 (v5.0 02/17/00 T)", CONSOLE_REGION_AUTO, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H10000 (v5.0 02/24/00 T)", CONSOLE_REGION_AUTO, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H30001 (v5.0 07/27/00 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001 (v5.0 07/27/00 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001 (v5.0 09/02/00 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H30002 (v5.0 09/02/00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, DTL-H30102 (v5.0 09/02/00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-30002/SCPH-30003/SCPH-30004 (v5.0 09/02/00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-18000 (GH-003) (v5.0 10/27/00 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-18000 (GH-008) (v5.0 10/27/00 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H30101 (v5.0 12/28/00 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001/SCPH-35001 (v5.0 12/28/00 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H30102 (v5.0 12/28/00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-30002/SCPH-30003/SCPH-30004/SCHP-35002/SCPH-35003/SCPH-35004 (v5.0 12/28/00 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H30000 (v5.0 01/18/01 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30000/SCPH-35000 (v5.0 01/18/01 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001R (v5.0 04/27/01 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30000 (v5.0 04/27/01 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001R (v5.0 07/04/01 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30002R/SCPH-30003R/SCPH-30004R (v5.0 07/04/01 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30001R (v5.0 10/04/01 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30002R/SCPH-30003R/SCPH-30004R (v5.0 10/04/01 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-30005R/SCPH-30006R/SCPH-30007R (v5.0 07/30/01 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-39001 (v5.0 02/07/02 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-39002/SCPH-39003/SCPH-39004 (v5.0 03/19/02 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-37000/SCPH-39000 (v5.0 04/26/02 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-39008 (v5.0 04/26/02 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-39005/SCPH-39006/SCPH-39007 (v5.0 04/26/02 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H50000 (v5.0 02/06/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50000/SCPH-55000 (v5.0 02/06/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H50002 (v5.0 02/27/03 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-50002/SCPH-50003/SCPH-50004 (v5.0 02/27/03 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H50001 (v5.0 03/25/03 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50001 (v5.0 03/25/03 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H50009 (v5.0 02/24/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DESR-5000/DESR-5100/DESR-7000/DESR-7100 (v5.0 10/28/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-55000 (v5.0 06/23/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H50001 (v5.0 06/23/03 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50001/SCPH-50010 (v5.0 06/23/03 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50002/SCPH-50003/SCPH-50004 (v5.0 06/23/03 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50006/SCPH-50007 (v5.0 06/23/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50005 (v5.0 06/23/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50008 (v5.0 06/23/03 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-50009 (v5.0 06/23/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50000 (v5.0 08/22/03 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-50004 (v5.0 08/22/03 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-50011 (v5.0 03/29/04 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-70000 (v5.0 06/14/04 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-70001/SCPH-70011/SCPH-70012 (v5.0 06/14/04 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-70002/SCPH-70003/SCPH-70004/SCPH-70008 (v5.0 06/14/04 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-70002 (v5.0 06/14/04 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, DTL-H70002 (v5.0 06/14/04 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
  {"PS2, SCPH-70005/SCPH-70006/SCPH-70007 (v5.0 06/14/04 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DESR-5500/DESR-5700/DESR-7500/DESR-7700 (v5.0 09/17/04 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H75000 (v5.0 06/20/05 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-75000 (v5.0 06/20/05 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H75000A (v5.0 06/20/05 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-75001/SCPH-75010 (v5.0 06/20/05 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-75002/SCPH-75003/SCPH-75004/SCPH-75008 (v5.0 06/20/05 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-75006 (v5.0 06/20/05 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-77000 (v5.0 02/10/06 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-77001/SCPH-77010 (v5.0 02/10/06 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-77002/SCPH-77003/SCPH-77004/SCPH-77008 (v5.0 02/10/06 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-77006/SCPH-77007 (v5.0 02/10/06 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H90000 (v5.0 09/05/06 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-79000/SCPH-90000 (v5.0 09/05/06 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, DTL-H90000 (v5.0 09/05/06 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-79001/SCPH-79010/SCPH-90001 (v5.0 09/05/06 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-79002/SCPH-79003/SCPH-79004/SCPH-79008/SCPH-90002/SCPH-90003/SCPH-90004 (v5.0 09/05/06 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-79006/SCPH-79007/SCPH-90006/SCPH-90007 (v5.0 09/05/06 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-90000 (v5.0 02/20/08 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-90001/SCPH-90010 (v5.0 02/20/08 A)", CONSOLE_REGION_NTSC_U, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-90002/SCPH-90003/SCPH-90004/SCPH-90008 (v5.0 02/20/08 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, SCPH-90005/SCPH-90006/SCPH-90007 (v5.0 02/20/08 J)", CONSOLE_REGION_NTSC_J, true, BIOS_FAST_BOOT_PATCH_TYPE2, 100, {0}},
  {"PS2, KDL-22PX300 (v5.0 04/15/10 E)", CONSOLE_REGION_PAL, true, BIOS_FAST_BOOT_PATCH_TYPE2, 150, {0}},
};

 /* Hex strings parallel to s_image_info_by_hash; populated in
 * bios_init_hash_table() (called automatically on first lookup). */
static const char* const s_image_info_hex[] = {
  "239665b1a3dade1b5a52c06338011044",
  "924e392ed05558ffdb115408c263dccf",
  "54847e693405ffeb0359c6287434cbef",
  "417b34706319da7cf001e76e40136c23",
  "e2110b8a2b97a8e0b857a45d32f7e187",
  "ca5cfc321f916756e3f0effbfaeba13b",
  "849515939161e62f6b866f6853006780",
  "dc2b9bf8da62ec93e868cfd29f0d067d",
  "cba733ceeff5aef5c32254f1d617fa62",
  "da27e8b6dab242d8f91a9b25d80c63b8",
  "57a06303dfa9cf9351222dfcbb4a29d9",
  "8dd7d5296a650fac7319bce665a6a53c",
  "490f666e1afb15b7362b406ed1cea246",
  "32736f17079d0b2b7024407c39bd3050",
  "8e4c14f567745eff2f0408c8129f72a6",
  "b84be139db3ee6cbd075630aa20a6553",
  "1e68c231d0896b7eadcad1d7d8e76129",
  "b9d9a0286c33dc6b7237bb13cd46fdee",
  "8abc1b549a4a80954addc48ef02c4521",
  "9a09ab7e49b422c007e6d54d7c49b965",
  "6e3735ff4c7dc899ee98981385f6f3d0",
  "b10f5e0e3d9eb60e5159690680b1e774",
  "de93caec13d1a141a40a79f5c86168d6",
  "476d68a94ccec3b9c8303bbd1daf2810",
  "d8f485717a5237285e4d7c5f881b7f32",
  "71f50ef4f4e17c163c78908e16244f7d",
  "32f2e4d5ff5ee11072a6bc45530f5765",
  "acf4730ceb38ac9d8c7d8e21f2614600",
  "acf9968c8f596d2b15f42272082513d1",
  "b1459d7446c69e3e97e6ace3ae23dd1c",
  "d3f1853a16c2ec18f3cd1ae655213308",
  "63e6fd9b3c72e0d7b920e80cf76645cd",
  "a20c97c02210f16678ca3010127caf36",
  "8db2fbbac7413bf3e7154c1e0715e565",
  "91c87cb2f2eb6ce529a2360f80ce2457",
  "3016b3dd42148a67e2c048595ca4d7ce",
  "b7fa11e87d51752a98b38e3e691cbf17",
  "f63bc530bd7ad7c026fcd6f7bd0d9525",
  "cee06bd68c333fc5768244eae77e4495",
  "0bf988e9c7aaa4c051805b0fa6eb3387",
  "8accc3c49ac45f5ae2c5db0adc854633",
  "6f9a6feb749f0533aaae2cc45090b0ed",
  "838544f12de9b0abc90811279ee223c8",
  "bb6bbc850458fff08af30e969ffd0175",
  "815ac991d8bc3b364696bead3457de7d",
  "b107b5710042abe887c0f6175f6e94bb",
  "ab55cceea548303c22c72570cfd4dd71",
  "18bcaadb9ff74ed3add26cdf709fff2e",
  "491209dd815ceee9de02dbbc408c06d6",
  "7200a03d51cacc4c14fcdfdbc4898431",
  "8359638e857c8bc18c3c18ac17d9cc3c",
  "352d2ff9b3f68be7e6fa7e6dd8389346",
  "d5ce2c7d119f563ce04bc04dbc3a323e",
  "0d2228e6fd4fb639c9c39d077a9ec10c",
  "72da56fccb8fcd77bba16d1b6f479914",
  "5b1f47fbeb277c6be2fccdd6344ff2fd",
  "315a4003535dfda689752cb25f24785c",
  "54ecde087258557e2ddb5c3ddb004028",
  "312ad4816c232a9606e56f946bc0678a",
  "666018ffec65c5c7e04796081295c6c7",
  "6e69920fa6eef8522a1d688a11e41bc6",
  "eb960de68f0c0f7f9fa083e9f79d0360",
  "8aa12ce243210128c5074552d3b86251",
  "240d4c5ddd4b54069bdc4a3cd2faf99d",
  "1c6cd089e6c83da618fbf2a081eb4888",
  "463d87789c555a4a7604e97d7db545d1",
  "ab9d49ad40ae49f19856ad187777b1b3",
  "35461cecaa51712b300b2d6798825048",
  "bd6415094e1ce9e05daabe85de807666",
  "2e70ad008d4ec8549aada8002fdf42fb",
  "50d5b97b57d8c9b6534adcb46c2027d4",
  "b53d51edc7fc086685e31b811dc32aad",
  "1b6e631b536247756287b916f9396872",
  "00da1b177096cfd2532c8fa22b43e667",
  "afde410bd026c16be605a1ae4bd651fd",
  "81f4336c1de607dd0865011c0447052e",
  "0eee5d1c779aa50e94edd168b4ebf42e",
  "d333558cc14561c1fdc334c75d5f37b7",
  "dc752f160044f2ed5fc1f4964db2a095",
  "7ebb4fc5eab6f79a27d76ac9aad392b2",
  "63ead1d74893bf7f36880af81f68a82d",
  "3e3e030c0f600442fa05b94f87a1e238",
  "1ad977bb539fc9448a08ab276a836bbc",
  "bf0078ba5e19d57eae18047407f3b6e5",
  "eb4f40fcf4911ede39c1bbfe91e7a89a",
  "9959ad7a8685cad66206e7752ca23f8b",
  "929a14baca1776b00869f983aa6e14d2",
  "573f7d4a430c32b3cc0fd0c41e104bbd",
  "df63a604e8bff5b0599bd1a6c2721bd0",
  "5b1ba4bb914406fae75ab8e38901684d",
  "cb801b7920a7d536ba07b6534d2433ca",
  "af60e6d1a939019d55e5b330d24b1c25",
  "549a66d0c698635ca9fa3ab012da7129",
  "5e2014472c88f74f7547d8c2c60eca45",
  "5de9d0d730ff1e7ad122806335332524",
  "21fe4cad111f7dc0f9af29477057f88d",
  "40c11c063b3b9409aa5e4058e984e30c",
  "80bbb237a6af9c611df43b16b930b683",
  "c37bce95d32b2be480f87dd32704e664",
  "80ac46fa7e77b8ab4366e86948e54f83",
  "21038400dc633070a78ad53090c53017",
  "dc69f0643a3030aaa4797501b483d6c4",
  "30d56e79d89fbddf10938fa67fe3f34e",
  "93ea3bcee4252627919175ff1b16a1d9", 
};

_Static_assert(sizeof(s_image_info_by_hash) / sizeof(s_image_info_by_hash[0]) ==
               sizeof(s_image_info_hex)     / sizeof(s_image_info_hex[0]),
               "BIOS info table and hex table must have equal length");

#define BIOS_INFO_TABLE_LEN (sizeof(s_image_info_by_hash) / sizeof(s_image_info_by_hash[0]))

static bool s_hash_table_initialized = false;

static void bios_init_hash_table(void)
{
  if (s_hash_table_initialized)
    return;
  for (size_t i = 0; i < BIOS_INFO_TABLE_LEN; i++)
    bios_make_hash_from_string(s_image_info_by_hash[i].hash, s_image_info_hex[i]);
  s_hash_table_initialized = true;
}

/* OpenBIOS is detected by signature, not hash, so it's separate. */
static const bios_image_info_t s_openbios_info = {
  "OpenBIOS", CONSOLE_REGION_AUTO, false, BIOS_FAST_BOOT_PATCH_UNSUPPORTED, 200, {0}
};
static const u8 s_openbios_signature[] = {'O','p','e','n','B','I','O','S'};
#define OPENBIOS_SIGNATURE_OFFSET 0x78

bool bios_supports_fast_boot(const bios_image_info_t* info)
{
  return info && info->fastboot_patch != BIOS_FAST_BOOT_PATCH_UNSUPPORTED;
}

bool bios_can_slow_boot_disc(const bios_image_info_t* info, disc_region_t disc_region)
{
  /* BIOSes without region checks can slow-boot anything; those with checks
   * can't slow-boot a mismatched disc. */
  if (!info->region_check)
    return true;

  /* Boot to BIOS for non-PS1 discs (e.g. audio CDs). */
  if (disc_region == DISC_REGION_NON_PS1)
    return true;

  switch (info->region)
  {
    case CONSOLE_REGION_NTSC_J: return (disc_region == DISC_REGION_NTSC_J);
    case CONSOLE_REGION_NTSC_U: return (disc_region == DISC_REGION_NTSC_U);
    case CONSOLE_REGION_PAL:    return (disc_region == DISC_REGION_PAL);
    default:                    return false;
  }
}

void bios_image_info_get_hash_string(tiny_string_t* out, const bios_hash_t hash)
{
  small_string_clear(&out->s);
  small_string_append_sprintf(&out->s,
    "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x",
    hash[0], hash[1], hash[2], hash[3], hash[4], hash[5], hash[6], hash[7], 
    hash[8], hash[9], hash[10], hash[11], hash[12], hash[13], hash[14], hash[15]);
}

void bios_image_destroy(bios_image_t* img)
{
  if (!img) return;
  free(img->data);
  img->data = NULL;
  img->data_size = 0;
  img->info = NULL;
}

bool bios_load_image_from_file(const char* filename, bios_image_t* out, Error* error)
{
  FILE* fp = fs_open_file(filename, "rb", error);
  if (!fp)
  {
    const char* base = NULL; u32 base_len = 0;
    path_get_file_name_cstr(filename, &base, &base_len);
    Error_add_prefix_fmt(error, "Failed to open BIOS '%.*s': ", (int)base_len, base);
    return false;
  }

  const s64 size = fs_fsize64(fp, error);
  if (size != (s64)BIOS_SIZE && size != (s64)BIOS_SIZE_PS2 && size != (s64)BIOS_SIZE_PS3)
  {
    const char* base = NULL; u32 base_len = 0;
    path_get_file_name_cstr(filename, &base, &base_len);
    Error_set_string_fmt(error,
      "BIOS image '%.*s' size mismatch, expecting either %u or %u bytes but got %lld bytes",
      (int)base_len, base, (unsigned)BIOS_SIZE, (unsigned)BIOS_SIZE_PS2, (long long)size);
    fclose(fp);
    return false;
  }

  /* Read whole file so we can hash the entire (possibly larger) image. */
  u8* data = NULL;
  size_t data_len = 0;
  if (!fs_read_binary_file(fp, &data, &data_len, error) || data_len < BIOS_SIZE)
  {
    free(data);
    fclose(fp);
    return false;
  }
  fclose(fp);

  bios_init_hash_table();

  bios_image_t img = {0};
  md5_digest_compute(data, data_len, img.hash);

  /* Only the first 512KB is mapped at 0x1FC0_0000. */
  img.data = data;
  img.data_size = BIOS_SIZE;
  img.info = bios_get_info_for_hash(img.data, img.data_size, img.hash);

  tiny_string_t hash_str; tiny_string_init(&hash_str);
  bios_image_info_get_hash_string(&hash_str, img.hash);
  const char* base = NULL; u32 base_len = 0;
  path_get_file_name_cstr(filename, &base, &base_len);
  DEV_LOG("Hash for BIOS '%.*s': %s", (int)base_len, base, small_string_c_str(&hash_str.s));
  small_string_destroy(&hash_str.s);

  *out = img;
  return true;
}

const bios_image_info_t* bios_get_info_for_hash(const u8* image, size_t image_len, const bios_hash_t hash)
{
  bios_init_hash_table();

  /* Check for OpenBIOS signature first. */
  if (image_len >= (OPENBIOS_SIGNATURE_OFFSET + sizeof(s_openbios_signature)) &&
      memcmp(&image[OPENBIOS_SIGNATURE_OFFSET], s_openbios_signature, sizeof(s_openbios_signature)) == 0)
  {
    return &s_openbios_info;
  }

  for (size_t i = 0; i < BIOS_INFO_TABLE_LEN; i++)
  {
    if (memcmp(s_image_info_by_hash[i].hash, hash, BIOS_HASH_SIZE) == 0)
      return &s_image_info_by_hash[i];
  }

  tiny_string_t hash_str; tiny_string_init(&hash_str);
  bios_image_info_get_hash_string(&hash_str, hash);
  WARNING_LOG("Unknown BIOS hash: %s", small_string_c_str(&hash_str.s));
  small_string_destroy(&hash_str.s);
  return NULL;
}

bool bios_is_valid_for_region(console_region_t console_region, console_region_t bios_region)
{
  return (console_region == CONSOLE_REGION_AUTO ||
          bios_region    == CONSOLE_REGION_AUTO ||
          bios_region    == console_region);
}

bool bios_patch_fast_boot(u8* image, u32 image_size, bios_fast_boot_patch_t type)
{
  /* Replace the shell entry point with a return back to the bootstrap.
   *   lui   at, 0x1F80                  -> 0x3C011F80
   *   lui   t2, 0x0300                  -> 0x3C0A0300
   *   sw    t2, 0x1814(at)  ; display on -> 0xAC2A1814
   *   jr    ra                          -> 0x03E00008
   *   nop                               -> 0x00000000 */
  static const u32 shell_replacement[] = {
    0x3C011F80u,
    0x3C0A0300u,
    0xAC2A1814u,
    0x03E00008u,
    0x00000000u, 
  };

  /* Type 1B/Type 2 use the same 5-instruction replacement, but for historical
   * reasons we write it at the actual shell code (Type 1) or at the routine
   * that calls the decompressor (Type 2). */
  u32 patch_offset = 0;
  if (type == BIOS_FAST_BOOT_PATCH_TYPE1)
  {
    /* Try the "new" Type 1B fast boot, where we patch the routine that
     * copies/decompresses the shell.  Saves ~2s of boot time because the
     * memcpy() runs out of uncached ROM. */
    static const char search_pattern[] =
      "e0 ff bd 27"  /* add sp, sp, -20 */
      "1c 00 bf af"  /* sw ra, 0xc(sp) */
      "20 00 a4 af"  /* sw a0, 0x20(sp) */
      "?? ?? 05 3c"  /* lui a1, 0xbfc1 */
      "?? ?? 06 3c"  /* lui a2, 0x6 */
      "?? ?? c6 34"  /* ori a2, a2, 0x7ff0 */
      "?? ?? a5 34"  /* ori a1, a1, 0x8000 */
      "?? ?? ?? 0f"; /* jal 0xbfc02b50 */
    size_t off = 0;
    if (string_util_byte_pattern_search(image, image_size, search_pattern,
                                        (u32)(sizeof(search_pattern) - 1), &off))
    {
      patch_offset = (u32)off;
      VERBOSE_LOG("Found Type 1B pattern at offset 0x%08X", patch_offset);
    }
    else
    {
      patch_offset = 0x18000u;
      INFO_LOG("Using Type 1A fast boot patch at offset 0x%08X.", patch_offset);
    }
  }
  else if (type == BIOS_FAST_BOOT_PATCH_TYPE2)
  {
    static const char search_pattern[] =
      "d8 ff bd 27"  /* add sp, sp, -28 */
      "1c 00 bf af"  /* sw ra, 0xc(sp) */
      "28 00 a4 af"  /* sw a0, 0x28(sp) */
      "?? ?? 06 3c"  /* lui a2, 0xbfc6 */
      "?? ?? c6 24"  /* addiu a2, -0x6bb8 */
      "c0 bf 04 3c"  /* lui a0, 0xbfc0 */
      "?? ?? ?? 0f"; /* jal 0xbfc58720 */
    static const u32 FALLBACK_OFFSET = 0x00052AFCu;
    size_t off = 0;
    if (string_util_byte_pattern_search(image, image_size, search_pattern,
                                        (u32)(sizeof(search_pattern) - 1), &off))
    {
      patch_offset = (u32)off;
      VERBOSE_LOG("Found Type 2 pattern at offset 0x%08X", patch_offset);
    }
    else
    {
      patch_offset = FALLBACK_OFFSET;
      WARNING_LOG("Failed to find Type 2 pattern in BIOS image. Using fallback offset of 0x%08X",
                  patch_offset);
    }
  }
  else
  {
    return false;
  }

  Assert((patch_offset + sizeof(shell_replacement)) <= image_size);
  memcpy(image + patch_offset, shell_replacement, sizeof(shell_replacement));
  return true;
}

bool bios_is_valid_psexe_header(const bios_psexe_header_t* header, size_t file_size)
{
  static const char expected_id[] = {'P','S','-','X',' ','E','X','E'};
  if (file_size < sizeof(expected_id) || memcmp(header->id, expected_id, sizeof(expected_id)) != 0)
    return false;

  if ((header->file_size + sizeof(bios_psexe_header_t)) > file_size)
  {
    WARNING_LOG("Incorrect file size in PS-EXE header: %u bytes should not be greater than %zu bytes",
                header->file_size, file_size - sizeof(bios_psexe_header_t));
  }
  return true;
}

disc_region_t bios_get_psexe_disc_region(const bios_psexe_header_t* header)
{
  static const char ntsc_u_id[] = "Sony Computer Entertainment Inc. for North America area";
  static const char ntsc_j_id[] = "Sony Computer Entertainment Inc. for Japan area";
  static const char pal_id[]    = "Sony Computer Entertainment Inc. for Europe area";

  if (memcmp(header->marker, ntsc_u_id, sizeof(ntsc_u_id) - 1) == 0)
    return DISC_REGION_NTSC_U;
  if (memcmp(header->marker, ntsc_j_id, sizeof(ntsc_j_id) - 1) == 0)
    return DISC_REGION_NTSC_J;
  if (memcmp(header->marker, pal_id, sizeof(pal_id) - 1) == 0)
    return DISC_REGION_PAL;
  return DISC_REGION_OTHER;
}

bool bios_get_image(console_region_t region, bios_image_t* out, Error* error)
{
  if (!emu_folders_get_bios)
  {
    Error_set_string(error, "BIOS folder API not available (emu_folders not yet ported).");
    return false;
  }

  small_string_stack_t bios_name; small_string_stack_init(&bios_name);
  if (core_get_string_setting_value)
  {
    const char* key;
    switch (region)
    {
      case CONSOLE_REGION_NTSC_J: key = "PathNTSCJ"; break;
      case CONSOLE_REGION_PAL:    key = "PathPAL";   break;
      case CONSOLE_REGION_NTSC_U:
      default:                    key = "PathNTSCU"; break;
    }
    core_get_string_setting_value("BIOS", key, "", &bios_name.s);
  }

  const char* bios_dir = emu_folders_get_bios();
  bool ok;
  if (small_string_empty(&bios_name.s))
  {
    ok = bios_find_image_in_directory(region, bios_dir, out, error);
  }
  else
  {
    small_string_stack_t path; small_string_stack_init(&path);
    path_combine_cstr2(&path.s, bios_dir, small_string_c_str(&bios_name.s));
    ok = bios_load_image_from_file(small_string_c_str(&path.s), out, error);
    small_string_destroy(&path.s);
  }

  if (ok && (!out->info || !bios_is_valid_for_region(region, out->info->region)))
  {
    WARNING_LOG("BIOS region %s does not match requested region %s. This may cause issues.",
                out->info ? region_name(out->info->region) : "UNKNOWN",
                region_name(region));
  }

  small_string_destroy(&bios_name.s);
  return ok;
}

bool bios_find_image_in_directory(console_region_t region, const char* directory,
                                  bios_image_t* out, Error* error)
{
  INFO_LOG("Searching for a %s BIOS in '%s'...", region_name(region), directory);
  Error_clear(error);

  fs_find_data_t* results = NULL;
  size_t result_count = 0;
  fs_find_files(directory, "*",
                FS_FIND_FILES | FS_FIND_HIDDEN_FILES | FS_FIND_RELATIVE_PATHS,
                &results, &result_count);

  bool          have_image = false;
  bios_image_t  image = {0};
  small_string_stack_t image_path; small_string_stack_init(&image_path);
  bool          image_region_match = false;

  for (size_t i = 0; i < result_count; i++)
  {
    const fs_find_data_t* fd = &results[i];
    if (fd->size != (s64)BIOS_SIZE && fd->size != (s64)BIOS_SIZE_PS2 && fd->size != (s64)BIOS_SIZE_PS3)
    {
      WARNING_LOG("Skipping '%s': incorrect size", fd->file_name);
      continue;
    }

    small_string_stack_t full_path; small_string_stack_init(&full_path);
    path_combine_cstr2(&full_path.s, directory, fd->file_name);

    bios_image_t found = {0};
    if (!bios_load_image_from_file(small_string_c_str(&full_path.s), &found, error))
    {
      small_string_destroy(&full_path.s);
      continue;
    }

    /* Don't let an unknown BIOS take precedence over a known one. */
    const bool region_match = (found.info && bios_is_valid_for_region(region, found.info->region));
    if (have_image &&
        ((image.info && !found.info) ||
         (image_region_match && !region_match) ||
         (image.info && found.info && image.info->priority < found.info->priority))) 
    {
      bios_image_destroy(&found);
      small_string_destroy(&full_path.s);
      continue;
    }

    if (have_image)
      bios_image_destroy(&image);
    image = found;
    small_string_assign(&image_path.s, &full_path.s);
    image_region_match = region_match;
    have_image = true;
    small_string_destroy(&full_path.s);
  }

  fs_free_find_data_array(results, result_count);

  if (!have_image)
  {
    if (Error_is_valid(error))
      Error_add_suffix(error, "\n\n");
    Error_add_suffix_fmt(error,
      "No BIOS image found for %s region.\n\ncupid-ps1 requires a PS1 or PS2 BIOS in order to "
      "run.\n\nFor legal reasons, you *must* obtain a BIOS from an actual PS1 unit that you own "
      "(borrowing doesn't count).\n\nOnce dumped, this BIOS image should be placed in the bios "
      "folder within the data directory.",
      region_name(region));
    small_string_destroy(&image_path.s);
    return false;
  }

  if (!image.info)
  {
    const char* base = NULL; u32 base_len = 0;
    path_get_file_name_cstr(small_string_c_str(&image_path.s), &base, &base_len);
    WARNING_LOG("Using unknown BIOS '%.*s'. This may crash.", (int)base_len, base);
  }

  small_string_destroy(&image_path.s);
  *out = image;
  return true;
}

bool bios_find_images_in_directory(const char* directory,
                                   bios_directory_entry_t** out_entries, size_t* out_count)
{
  fs_find_data_t* files = NULL;
  size_t file_count = 0;
  fs_find_files(directory, "*",
                FS_FIND_FILES | FS_FIND_HIDDEN_FILES | FS_FIND_RELATIVE_PATHS,
                &files, &file_count);

  bios_directory_entry_t* entries = NULL;
  size_t entries_cap = 0, entries_len = 0;

  for (size_t i = 0; i < file_count; i++)
  {
    const fs_find_data_t* fd = &files[i];
    if (fd->size != (s64)BIOS_SIZE && fd->size != (s64)BIOS_SIZE_PS2 && fd->size != (s64)BIOS_SIZE_PS3)
      continue;

    small_string_stack_t full_path; small_string_stack_init(&full_path);
    path_combine_cstr2(&full_path.s, directory, fd->file_name);

    bios_image_t img = {0};
    if (!bios_load_image_from_file(small_string_c_str(&full_path.s), &img, NULL))
    {
      small_string_destroy(&full_path.s);
      continue;
    }

    if (entries_len == entries_cap)
    {
      entries_cap = entries_cap ? (entries_cap * 2) : 8;
      entries = (bios_directory_entry_t*)realloc(entries, entries_cap * sizeof(bios_directory_entry_t));
      if (!entries) Panic("Out of memory in bios_find_images_in_directory");
    }
    entries[entries_len].filename = strdup(fd->file_name);
    entries[entries_len].info     = img.info;
    entries_len++;

    bios_image_destroy(&img);
    small_string_destroy(&full_path.s);
  }

  fs_free_find_data_array(files, file_count);

  *out_entries = entries;
  *out_count   = entries_len;
  return entries_len > 0;
}

void bios_free_image_listing(bios_directory_entry_t* entries, size_t count)
{
  if (!entries) return;
  for (size_t i = 0; i < count; i++)
    free(entries[i].filename);
  free(entries);
}

bool bios_has_any_images(void)
{
  if (!emu_folders_get_bios)
    return false;

  bios_image_t img = {0};
  bool ok = bios_find_image_in_directory(CONSOLE_REGION_AUTO, emu_folders_get_bios(), &img, NULL);
  if (ok) bios_image_destroy(&img);
  return ok;
}
