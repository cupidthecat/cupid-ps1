/* cupid-ps1 executable entry register and payload regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/bios/bios.h"

static void check_entry(u32 base, u32 offset, bool set_pc)
{
  const u32 loop[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0x80020000, loop, 2);
  g_cpu_state.regs.named.sp = 0x801F9000;
  g_cpu_state.regs.named.fp = 0xA0A10000;
  g_cpu_state.regs.named.gp = 0xABCD0000;
  u8 image[sizeof(bios_psexe_header_t) + sizeof(loop)] = {0};
  bios_psexe_header_t header = {0};
  memcpy(header.id, "PS-X EXE", sizeof(header.id));
  header.initial_pc        = 0x80010000;
  header.initial_gp        = 0x13572468;
  header.load_address      = 0x80010000;
  header.file_size         = sizeof(loop);
  header.memfill_start     = 0x80030000;
  header.memfill_size      = 16;
  header.initial_sp_base   = base;
  header.initial_sp_offset = offset;
  memcpy(image, &header, sizeof(header));
  memcpy(image + sizeof(header), loop, sizeof(loop));
  u8* ram = bus_get_unprotected_ram_pointer();
  memset(ram + 0x30000, 0xCD, 20);
  Error error;
  Error_init(&error);
  EXPECT_TRUE(bus_inject_executable(image, sizeof(image), set_pc, &error));
  EXPECT_EQ(memcmp(ram + 0x10000, loop, sizeof(loop)), 0);
  for (u32 i = 0; i < 16; i++) EXPECT_EQ(ram[0x30000 + i], 0);
  EXPECT_EQ(ram[0x30010], 0xCD);
  EXPECT_EQ(g_cpu_state.pc, set_pc ? header.initial_pc : 0x80020000);
  EXPECT_EQ(g_cpu_state.regs.named.gp, set_pc ? header.initial_gp : 0xABCD0000);
  EXPECT_EQ(g_cpu_state.regs.named.sp, set_pc && base ? base + offset : 0x801F9000);
  EXPECT_EQ(g_cpu_state.regs.named.fp, set_pc && base ? base + offset : 0xA0A10000);
  Error_destroy(&error);
  finish_cpu();
}

TEST(ExecutableStack, zero_base_preserves_distinct_stack_and_frame_pointers)
{
  check_entry(0, 0, true);
}

TEST(ExecutableStack, zero_base_ignores_nonzero_offsets)
{
  check_entry(0, 0x100, true);
  check_entry(0, 0xFFFFFFFF, true);
}

TEST(ExecutableStack, nonzero_base_adds_offset_to_both_pointers)
{
  check_entry(0x801FC000, 0, true);
  check_entry(0x801FC000, 0x28, true);
}

TEST(ExecutableStack, nonzero_base_can_wrap_to_zero)
{
  check_entry(0xFFFFFFF0, 0x10, true);
}

TEST(ExecutableStack, load_only_preserves_entry_registers)
{
  check_entry(0, 0x100, false);
  check_entry(0x801FC000, 0x28, false);
  check_entry(0xFFFFFFF0, 0x10, false);
}
