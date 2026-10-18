/* cupid-ps1 SPU DMA read boundary tests. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "core/dma/dma.h"
#include "core/interrupts/interrupt_controller.h"
#include "core/spu/spu.h"

enum { STATE_CAPACITY = SPU_RAM_SIZE + 65536, TRANSFER_START = 0x10000 };

typedef struct {
  u16 status;
  u32 address, head, tail, size;
  u16 halfwords[32];
} transfer_snapshot_t;

static transfer_snapshot_t read_transfer(void)
{
  u8* bytes = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(bytes);
  state_wrapper_t state;
  state_wrapper_init_write(&state, bytes, STATE_CAPACITY, SAVE_STATE_VERSION);
  EXPECT_TRUE(spu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  const size_t length = state_wrapper_get_position(&state);
  EXPECT_TRUE(length >= SPU_RAM_SIZE + 76);
  transfer_snapshot_t result;
  state_wrapper_init_read(&state, bytes, length, SAVE_STATE_VERSION);
  state_wrapper_skip_bytes(&state, 6);
  state_wrapper_do_u16(&state, &result.status);
  state_wrapper_skip_bytes(&state, 2);
  state_wrapper_do_u32(&state, &result.address);
  state_wrapper_set_position(&state, length - SPU_RAM_SIZE - 76);
  state_wrapper_do_array(&state, result.halfwords, sizeof(u16), 32);
  state_wrapper_do_u32(&state, &result.head);
  state_wrapper_do_u32(&state, &result.tail);
  state_wrapper_do_u32(&state, &result.size);
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_TRUE(result.head < 32 && result.tail < 32);
  free(bytes);
  return result;
}

static void restore_transfer(void)
{
  u8  cpu[16384], dma[4096], events[4096];
  u8* sound = calloc(1, STATE_CAPACITY);
  u8* check = calloc(1, STATE_CAPACITY);
  EXPECT_NOT_NULL(sound);
  EXPECT_NOT_NULL(check);
  struct {
    bool (*do_state)(state_wrapper_t*);
    u8*    bytes;
    size_t capacity, length;
  } components[] = {{cpu_do_state, cpu, sizeof(cpu), 0},
                    {spu_do_state, sound, STATE_CAPACITY, 0},
                    {dma_do_state, dma, sizeof(dma), 0},
                    {timing_events_do_state, events, sizeof(events), 0}};
  state_wrapper_t state;
  for (u32 i = 0; i < 4; i++) {
    state_wrapper_init_write(&state, components[i].bytes, components[i].capacity, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    components[i].length = state_wrapper_get_position(&state);
  }
  spu_reset();
  for (u32 i = 0; i < 4; i++) {
    state_wrapper_init_read(&state, components[i].bytes, components[i].length, SAVE_STATE_VERSION);
    EXPECT_TRUE(components[i].do_state(&state));
    EXPECT_FALSE(state_wrapper_has_error(&state));
    EXPECT_EQ(state_wrapper_get_position(&state), components[i].length);
  }
  state_wrapper_init_write(&state, check, STATE_CAPACITY, SAVE_STATE_VERSION);
  EXPECT_TRUE(spu_do_state(&state));
  EXPECT_FALSE(state_wrapper_has_error(&state));
  EXPECT_EQ(state_wrapper_get_position(&state), components[1].length);
  EXPECT_EQ(memcmp(sound, check, components[1].length), 0);
  free(check);
  free(sound);
}

static void begin_transfer(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, const u32* program, size_t count,
                           u32 available, u32 rotation, u32 pattern)
{
  start_cpu_with_fastmem(mode, fastmem, 0xA0010000, program, count);
  interrupt_controller_reset();
  dma_initialize();
  spu_initialize();
  for (u32 i = 0; i < 128; i++) {
    const u16 value = (u16)((pattern ? 0xC931u : 0x2547u) ^ (i * 0x137u));
    memcpy(spu_get_writable_ram() + TRANSFER_START + 2 * i, &value, sizeof(value));
  }
  spu_write_register(0x1A6, TRANSFER_START / 8);
  spu_write_register(0x1AA, 0x30);
  spu_execute(32 * 16);
  EXPECT_EQ(read_transfer().size, 32);
  if (rotation) {
    u32 scratch[13];
    spu_dma_read(scratch, 13);
    EXPECT_EQ(read_transfer().size, 6);
    spu_execute(32 * 16);
    EXPECT_EQ(read_transfer().size, 32);
  }
  if (available < 32) {
    u32 scratch[16];
    spu_dma_read(scratch, (32 - available) / 2);
  }
  const transfer_snapshot_t before = read_transfer();
  EXPECT_EQ(before.size, available);
  EXPECT_EQ(before.address, TRANSFER_START + 2 * (32 + (rotation ? 26 : 0)));
  const u32 first = 32 - available + (rotation ? 26 : 0);
  for (u32 i = 0; i < available; i++) {
    const u16 expected = (u16)((pattern ? 0xC931u : 0x2547u) ^ ((first + i) * 0x137u));
    EXPECT_EQ(before.halfwords[(before.head + i) % 32], expected);
  }
  EXPECT_EQ(before.status & 0x780, available == 32 ? 0x180 : 0x400);
  EXPECT_EQ(dma_dbg_request(4), available == 32);
}

static void end_transfer(void)
{
  spu_shutdown();
  dma_shutdown();
  finish_cpu();
}

static bool check_read(const transfer_snapshot_t* before, const transfer_snapshot_t* after, const u32* words, u32 count,
                       u32* wrong)
{
  const u32 consumed = before->size < count * 2 ? before->size : count * 2;
  u16       fill     = 0;
  *wrong             = 0;
  for (u32 i = 0; i < count * 2; i++) {
    if (i < before->size) fill = before->halfwords[(before->head + i) % 32];
    *wrong += (u16)(words[i / 2] >> (16 * (i & 1))) != fill;
  }
  bool equal = after->size == before->size - consumed && after->head == (before->head + consumed) % 32 &&
               after->tail == before->tail && after->address == before->address;
  for (u32 i = 0; i < 32; i++)
    equal &= after->halfwords[i] == before->halfwords[i];
  const u16 expected_flags = (after->size == 32 ? 0x180 : 0) | (after->size < 32 ? 0x400 : 0);
  equal &= (after->status & 0x780) == expected_flags;
  equal &= dma_dbg_request(4) == (after->size == 32);
  return equal && *wrong == 0;
}

static void check_direct(bool restored)
{
  const u32 occupancy[]  = {0, 2, 4, 16, 30, 32};
  const u32 requests[]   = {0, 1, 2, 8, 16, 17, 32, 33};
  const u32 program[]    = {0x1000FFFF, 0};
  u32       observations = 0, failures = 0;
  for (u32 slot = 0; slot < 6; slot++) {
    for (u32 request = 0; request < 8; request++) {
      for (u32 rotation = 0; rotation < 2; rotation++) {
        for (u32 pattern = 0; pattern < 2; pattern++) {
          begin_transfer(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, program, 2, occupancy[slot],
                         rotation, pattern);
          if (restored) restore_transfer();
          const transfer_snapshot_t before = read_transfer();
          u32                       words[35];
          for (u32 i = 0; i < 35; i++)
            words[i] = 0xA55A9911;
          spu_dma_read(words + 1, requests[request]);
          const transfer_snapshot_t after = read_transfer();
          u32                       wrong;
          bool                      equal = check_read(&before, &after, words + 1, requests[request], &wrong);
          equal &= words[0] == 0xA55A9911;
          for (u32 i = requests[request] + 1; i < 35; i++)
            equal &= words[i] == 0xA55A9911;
          printf("spu-dma-read restored=%u available=%u requested=%u rotation=%u pattern=%u before=%u/%u/%u "
                 "after=%u/%u/%u status=%04X/%04X wrong=%u complete=%u\n",
                 restored, occupancy[slot], requests[request], rotation, pattern, before.head, before.tail, before.size,
                 after.head, after.tail, after.size, before.status, after.status, wrong, equal);
          failures += !equal;
          observations++;
          end_transfer();
        }
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 192);
  EXPECT_EQ(failures, 0);
}

TEST(SpuDmaRead, fresh_boundaries)
{
  check_direct(false);
}
TEST(SpuDmaRead, restored_boundaries)
{
  check_direct(true);
}

static void check_guest(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 requests[]   = {1, 16, 17, 32, 33};
  u32       observations = 0, failures = 0;
  for (u32 restored = 0; restored < 2; restored++) {
    for (u32 request = 0; request < 5; request++) {
      for (u32 pattern = 0; pattern < 2; pattern++) {
        const u32 program[] = {0x3C081F80, 0x350810C0, 0x3C0EA005, 0x8DC90000, 0x8DCA0004, 0x8DCB0008,
                               0x8DCC000C, 0,          0xAD090000, 0xAD0A0004, 0xAD0B0030, 0xAD0C0008,
                               0x24160001, 0x3C0DA003, 0xADB60000, 0x1000FFFF, 0};
        begin_transfer(mode, fastmem, program, sizeof(program) / sizeof(program[0]), 32, 0, pattern);
        g_settings.cpu_recompiler_block_linking = linking;
        if (restored) restore_transfer();
        const transfer_snapshot_t before    = read_transfer();
        const u32                 requested = requests[request];
        const u32                 params[]  = {0x40004, 0x10000 | requested, 0x80000, 0x01000200};
        memcpy(bus_get_unprotected_ram_pointer() + 0x50000, params, sizeof(params));
        memset(bus_get_unprotected_ram_pointer() + 0x40000, 0xA5, 35 * sizeof(u32));
        memset(bus_get_unprotected_ram_pointer() + 0x30000, 0xCC, sizeof(u32));
        run_to_pc(0xA0010000 + (sizeof(program) / sizeof(program[0]) - 2) * 4);
        u32 completed, words[35];
        memcpy(&completed, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(completed));
        memcpy(words, bus_get_unprotected_ram_pointer() + 0x40000, sizeof(words));
        EXPECT_EQ(completed, 1);
        EXPECT_EQ(dma_dbg_base_address(4), 0x40004 + requested * 4);
        EXPECT_EQ(dma_dbg_block_control(4) >> 16, 0);
        EXPECT_EQ(dma_dbg_channel_control(4) & (1u << 24), 0);
        const transfer_snapshot_t after = read_transfer();
        u32                       wrong;
        bool                      equal = check_read(&before, &after, words + 1, requested, &wrong);
        equal &= words[0] == 0xA5A5A5A5;
        for (u32 i = requested + 1; i < 35; i++)
          equal &= words[i] == 0xA5A5A5A5;
        const cpu_execution_stats_t          stats  = *cpu_execution_stats_get();
        const cpu_cached_interpreter_stats_t cached = cpu_code_cache_get_cached_interpreter_stats();
        if (mode == CPU_EXECUTION_MODE_CACHED_INTERPRETER) {
          EXPECT_TRUE(cached.blocks_created > 0 && cached.blocks_executed > 0 && cached.instructions_executed > 0);
          EXPECT_EQ(cached.fallback_instructions, 0);
        }
        if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
          EXPECT_TRUE(stats.native_blocks_completed > 0);
          EXPECT_EQ(stats.interpreted_instructions, stats.native_dispatch_delay_slots);
          if (fastmem == CPU_FASTMEM_MODE_LUT) {
            EXPECT_TRUE(stats.native_lut_loads >= 4);
            EXPECT_TRUE(stats.native_lut_stores >= 1);
          }
        }
        printf("spu-dma-read-cpu mode=%u fastmem=%u linking=%u restored=%u requested=%u pattern=%u "
               "before=%u/%u/%u after=%u/%u/%u status=%04X/%04X wrong=%u complete=%u\n",
               mode, fastmem, linking, restored, requested, pattern, before.head, before.tail, before.size, after.head,
               after.tail, after.size, before.status, after.status, wrong, equal);
        failures += !equal;
        observations++;
        end_transfer();
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 20);
  EXPECT_EQ(failures, 0);
}

#define SPU_READ_CPU_CASE(name, mode, fastmem, linking) \
  TEST(SpuDmaReadCpu, name)                             \
  {                                                     \
    check_guest(mode, fastmem, linking);                \
  }
SPU_READ_CPU_CASE(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_READ_CPU_CASE(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_READ_CPU_CASE(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
SPU_READ_CPU_CASE(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
SPU_READ_CPU_CASE(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
SPU_READ_CPU_CASE(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
SPU_READ_CPU_CASE(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
SPU_READ_CPU_CASE(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
