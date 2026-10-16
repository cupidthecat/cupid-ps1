/* Execute guest loads and stores against both serial MODE registers. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/input/pad.h"
#include "core/serial/sio.h"

static u32 memory_instruction(u32 opcode, u32 base, u32 value, u32 offset)
{
  return (opcode << 26) | (base << 21) | (value << 16) | offset;
}

static void check_mode_bus(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 stores[] = {0x28, 0x29, 0x2B}; /* sb, sh, sw */
  const u32 loads[]  = {0x24, 0x25, 0x23}; /* lbu, lhu, lw */
  const u32 values[] = {0x12345678, 0xFFFFFFFF};
  for (u32 port = 0; port < 2; port++) {
    for (u32 width = 0; width < 3; width++) {
      for (u32 sample = 0; sample < 2; sample++) {
        u32    words[24];
        size_t count   = 0;
        words[count++] = memory_instruction(stores[width], 8, 9, 0);
        for (u32 read_width = 0; read_width < 3; read_width++) {
          words[count++] = memory_instruction(loads[read_width], 8, 2, 0);
          words[count++] = 0; /* load delay */
          words[count++] = memory_instruction(0x2B, 16, 2, read_width * 4);
        }
        words[count++] = memory_instruction(0x24, 8, 2, 1); /* upper byte */
        words[count++] = 0;
        words[count++] = memory_instruction(0x2B, 16, 2, 12);
        words[count++] = memory_instruction(0x28, 8, 10, 1); /* upper-byte write */
        words[count++] = memory_instruction(0x25, 8, 2, 0);
        words[count++] = 0;
        words[count++] = memory_instruction(0x2B, 16, 2, 16);
        words[count++] = memory_instruction(0x24, 8, 2, 1);
        words[count++] = 0;
        words[count++] = memory_instruction(0x2B, 16, 2, 20);
        words[count++] = 0x1000FFFF;
        words[count++] = 0;
        start_cpu_with_fastmem(mode, fastmem, 0x80010000, words, count);
        pad_initialize();
        sio_initialize();
        g_cpu_state.regs.r[8]  = 0x1F801048 + port * 0x10;
        g_cpu_state.regs.r[9]  = values[sample];
        g_cpu_state.regs.r[10] = 1;
        g_cpu_state.regs.r[16] = 0x80030000;
        if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
        run_slice(500);
        u32 result[6];
        memcpy(result, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(result));
        const u32 expected = sample ? (port ? 0x00FF : 0x013F) : (port ? 0x0078 : 0x0038);
        EXPECT_EQ(result[0], expected & 0xFF);
        EXPECT_EQ(result[1], expected);
        EXPECT_EQ(result[2], expected);
        EXPECT_EQ(result[3], expected >> 8);
        EXPECT_EQ(result[4], port ? 0 : 0x0100);
        EXPECT_EQ(result[5], port ? 0 : 1);
        if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
          EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
          EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
        }
        sio_shutdown();
        pad_shutdown();
        finish_cpu();
      }
    }
  }
}

TEST(SerialModeBus, interpreter_access_widths)
{
  check_mode_bus(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}
TEST(SerialModeBus, cached_access_widths)
{
  check_mode_bus(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}
TEST(SerialModeBus, native_access_widths)
{
  check_mode_bus(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED);
}
TEST(SerialModeBus, mmap_access_widths)
{
  check_mode_bus(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP);
}
