 /*
 * (per STATUS.md):
 *   - MSF skip condition: digit-skip uses `< '0' || > '9'`, not a
 *     contradictory `< '0' && <= '9'`.
 *
 * Both surface in cue_parser_parse_msf().  We feed boundary inputs and
 * verify acceptance/rejection.
 */

#include "tests/test_harness.h"

#include "common/error.h"
#include "util/cue_parser.h"

#include <string.h>

static void parse_string(const char* src, bool* out_ok)
{
  cue_parser_file_t f;
  cue_parser_file_init(&f);
  Error err = ERROR_INIT;
  *out_ok = cue_parser_file_parse_buffer(&f, src, &err);
  Error_destroy(&err);
  cue_parser_file_destroy(&f);
}

TEST(CueParser, ValidMSF)
{
  /* All-digit MSF inside legal range (frame 74). */
  const char* src =
    "FILE \"track1.bin\" BINARY\n"
    "  TRACK 01 MODE2/2352\n"
    "    INDEX 01 00:00:00\n";
  bool ok = false;
  parse_string(src, &ok);
  EXPECT_TRUE(ok);
}

TEST(CueParser, MSFFrameBoundary74)
{
  /* Frame=74 must be accepted (CD-DA spec max). */
  const char* src =
    "FILE \"track1.bin\" BINARY\n"
    "  TRACK 01 MODE2/2352\n"
    "    INDEX 01 00:00:74\n";
  bool ok = false;
  parse_string(src, &ok);
  EXPECT_TRUE(ok);
}

TEST(CueParser, MSFFrameOutOfRange75)
{
  const char* src =
    "FILE \"track1.bin\" BINARY\n"
    "  TRACK 01 MODE2/2352\n"
    "    INDEX 01 00:00:75\n";
  bool ok = false;
  parse_string(src, &ok);
  EXPECT_FALSE(ok);
}

TEST(CueParser, MSFNonDigitRejected)
{
  /* Non-digit token char should be rejected by the corrected skip
   * condition (vs a contradictory check that would have silently
   * passed). */
  const char* src =
    "FILE \"track1.bin\" BINARY\n"
    "  TRACK 01 MODE2/2352\n"
    "    INDEX 01 0X:00:00\n";
  bool ok = false;
  parse_string(src, &ok);
  EXPECT_FALSE(ok);
}
