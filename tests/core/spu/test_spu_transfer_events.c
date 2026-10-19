/* cupid-ps1 guest DMA polling without SPU status reads. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/spu/spu.h"
#include "tests/core/spu/spu_state_fixture.h"

static void check_dma_polling(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 program[] = {
      0x3C081F80, 0x350810C0, 0x3C0EA005, 0x8DC90000, 0x8DCA0004, 0x8DCB0008, 0x8DCC000C, 0,
      0xAD090000, 0xAD0A0004, 0xAD0B0030, 0xAD0C0008,
      0x8D020008, 0, 0x00021602, 0x30420001, 0x1440FFFB, 0x26310001,
      0x3C0DA003, 0x24160001, 0xADB60000, 0x1000FFFF, 0};
  for (u32 read = 0; read < 2; read++) {
    start_cpu_with_fastmem(mode, fastmem, 0xA0010000, program, sizeof(program) / sizeof(program[0]));
    g_settings.cpu_recompiler_block_linking = linking;
    interrupt_controller_reset();
    dma_initialize();
    spu_initialize();
    const u32 params[] = {0x40000, 0x00100010, 0x80000, read ? 0x01000200 : 0x01000201};
    memcpy(bus_get_unprotected_ram_pointer() + 0x50000, params, sizeof(params));
    u32 expected[256];
    for (u32 i = 0; i < 256; i++) expected[i] = 0x17C9AB31 ^ (i * 0x387D1369u);
    if (read) {
      memcpy(spu_get_writable_ram() + 0x10000, expected, sizeof(expected));
      memset(bus_get_unprotected_ram_pointer() + 0x40000, 0xA5, sizeof(expected));
    } else {
      memcpy(bus_get_unprotected_ram_pointer() + 0x40000, expected, sizeof(expected));
    }
    spu_write_register(0x1A6, 0x10000 / 8);
    spu_write_register(0x1AA, read ? 0x30 : 0x20);
    run_slice(1000);
    spu_execution_snapshot_t midway = save_spu_execution(SAVE_STATE_VERSION);
    EXPECT_TRUE((dma_dbg_block_control(4) >> 16) > 0);
    run_slice(50000);
    spu_execution_snapshot_t final = save_spu_execution(SAVE_STATE_VERSION);
    load_spu_execution(&midway);
    compare_spu_execution(&midway);
    run_slice(50000);
    compare_spu_execution(&final);
    free_spu_execution(&final);
    free_spu_execution(&midway);
    u32 completed;
    memcpy(&completed, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(completed));
    printf("spu-poll mode=%u fastmem=%u linking=%u read=%u complete=%u polls=%u remaining=%u\n",
           mode, fastmem, linking, read, completed, g_cpu_state.regs.r[17], dma_dbg_block_control(4) >> 16);
    fflush(stdout);
    EXPECT_EQ(completed, 1);
    EXPECT_TRUE(g_cpu_state.regs.r[17] > 1);
    EXPECT_EQ(dma_dbg_channel_control(4) & (1u << 24), 0);
    EXPECT_EQ(dma_dbg_block_control(4) >> 16, 0);
    EXPECT_EQ(memcmp(read ? bus_get_unprotected_ram_pointer() + 0x40000 : spu_get_ram() + 0x10000,
                     expected, sizeof(expected)), 0);
    const cpu_execution_stats_t stats = *cpu_execution_stats_get();
    if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) {
      const cpu_cached_interpreter_stats_t cached = cpu_code_cache_get_cached_interpreter_stats();
      EXPECT_TRUE(cached.blocks_executed > 0);
      EXPECT_EQ(cached.fallback_instructions, 0);
    }
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(stats.native_blocks_completed > 0);
      EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
      if (fastmem == CPU_FASTMEM_MODE_LUT) EXPECT_TRUE(stats.native_lut_loads >= 4 && stats.native_lut_stores > 0);
    }
    spu_shutdown();
    dma_shutdown();
    finish_cpu();
  }
}

#define TRANSFER_EVENT_CASE(name, mode, fastmem, linking) \
  TEST(SpuTransferEvents, name)                         \
  { check_dma_polling(mode, fastmem, linking); }
TRANSFER_EVENT_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
TRANSFER_EVENT_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
TRANSFER_EVENT_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
TRANSFER_EVENT_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
TRANSFER_EVENT_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
TRANSFER_EVENT_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
TRANSFER_EVENT_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
TRANSFER_EVENT_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
