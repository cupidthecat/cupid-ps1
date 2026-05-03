 /*
 *
 * Byte-walk tests for the dispatcher prelude emitted by
 * src/core/x64_backend.c::emit_asm_functions.
 *
 * We do not run the prelude — running would require the full system to be
 * initialized (g_cpu_state populated, timing events live, code-cache LUT
 * filled).  Instead we emit it into a writable buffer and assert that:
 *
 *   1. emit returns a sensible byte count
 *   2. the six dispatcher globals are populated and point inside the buffer
 *   3. the first byte at each global matches the expected x86 opcode
 *
 * End-to-end correctness is exercised by booting a BIOS in recompiler mode.
 */

#include "tests/test_harness.h"

#include "core/cpu_code_cache.h"
#include "core/x64_backend.h"

#include <stdint.h>
#include <string.h>

/* Prelude is well under 256 bytes today; give plenty of slack. */
static u8 g_prelude_buf[1024];

TEST(X64Backend, dispatcher_prelude_emits_and_populates_globals) {
  memset(g_prelude_buf, 0xCC, sizeof(g_prelude_buf));
  /* Reset globals so the test is independent of any prior state. */
  g_enter_recompiler            = NULL;
  g_dispatcher                  = NULL;
  g_run_events_and_dispatch     = NULL;
  g_compile_or_revalidate_block = NULL;
  g_discard_and_recompile_block = NULL;
  g_interpret_block             = NULL;

  const u32 size = x64_backend_emit_dispatcher_for_test(g_prelude_buf, sizeof(g_prelude_buf));

  EXPECT_TRUE(size > 0);
  EXPECT_TRUE(size < sizeof(g_prelude_buf));

  /* All six globals should be inside the buffer. */
  const u8* const base = g_prelude_buf;
  const u8* const end  = base + size;

  EXPECT_TRUE((const u8*)g_enter_recompiler            >= base &&
              (const u8*)g_enter_recompiler            <  end);
  EXPECT_TRUE((const u8*)g_run_events_and_dispatch     >= base &&
              (const u8*)g_run_events_and_dispatch     <  end);
  EXPECT_TRUE((const u8*)g_dispatcher                  >= base &&
              (const u8*)g_dispatcher                  <  end);
  EXPECT_TRUE((const u8*)g_compile_or_revalidate_block >= base &&
              (const u8*)g_compile_or_revalidate_block <  end);
  EXPECT_TRUE((const u8*)g_discard_and_recompile_block >= base &&
              (const u8*)g_discard_and_recompile_block <  end);
  EXPECT_TRUE((const u8*)g_interpret_block             >= base &&
              (const u8*)g_interpret_block             <  end);

  /* g_enter_recompiler must start with REX.W + SUB r/m64, imm8 (= 48 83 EC 08). */
  const u8* enter = (const u8*)g_enter_recompiler;
  EXPECT_EQ(enter[0], 0x48);
  EXPECT_EQ(enter[1], 0x83);

  /* g_run_events_and_dispatch begins with `mov rax, imm64; call rax`
   * (call_abs64 helper).  First two bytes: 0x48 0xB8 (MOVABS RAX, ...). */
  const u8* run_evt = (const u8*)g_run_events_and_dispatch;
  EXPECT_EQ(run_evt[0], 0x48);
  EXPECT_EQ(run_evt[1], 0xB8);

  /* g_dispatcher begins with `mov edi, [rbp + disp]`.
   * disp != 0 → mod=01 (disp8) or mod=10 (disp32); reg=111 (RDI=7), rm=101 (RBP=5).
   * So byte 0 = 0x8B (MOV r32, r/m32), byte 1 = 0x7D (mod01 reg111 rm101) for disp8
   *                                          or 0xBD (mod10 reg111 rm101) for disp32.
   * The pending_ticks/downcount/pc fields all sit at low offsets in
   * cpu_state_t (well under 128 bytes), so disp8. */
  const u8* disp = (const u8*)g_dispatcher;
  EXPECT_EQ(disp[0], 0x8B);
  /* ModRM is one of the two forms above. */
  EXPECT_TRUE(disp[1] == 0x7D || disp[1] == 0xBD);

  /* g_compile_or_revalidate_block / g_discard_and_recompile_block both
   * begin the same way (mov edi, [rbp+pc]). */
  const u8* compile = (const u8*)g_compile_or_revalidate_block;
  EXPECT_EQ(compile[0], 0x8B);
  EXPECT_TRUE(compile[1] == 0x7D || compile[1] == 0xBD);

  const u8* discard = (const u8*)g_discard_and_recompile_block;
  EXPECT_EQ(discard[0], 0x8B);
  EXPECT_TRUE(discard[1] == 0x7D || discard[1] == 0xBD);

  /* g_interpret_block begins with `call <abs64>` = MOVABS RAX, ... */
  const u8* interp = (const u8*)g_interpret_block;
  EXPECT_EQ(interp[0], 0x48);
  EXPECT_EQ(interp[1], 0xB8);
}

TEST(X64Backend, dispatcher_prelude_globals_are_distinct) {
  memset(g_prelude_buf, 0xCC, sizeof(g_prelude_buf));
  (void)x64_backend_emit_dispatcher_for_test(g_prelude_buf, sizeof(g_prelude_buf));

  /* Each entry point must occupy a distinct location. */
  EXPECT_TRUE(g_enter_recompiler            != (void*)g_run_events_and_dispatch);
  EXPECT_TRUE(g_run_events_and_dispatch     != g_dispatcher);
  EXPECT_TRUE(g_dispatcher                  != g_compile_or_revalidate_block);
  EXPECT_TRUE(g_compile_or_revalidate_block != g_discard_and_recompile_block);
  EXPECT_TRUE(g_discard_and_recompile_block != g_interpret_block);
}
