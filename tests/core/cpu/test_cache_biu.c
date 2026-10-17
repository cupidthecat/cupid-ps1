/* cupid-ps1 isolated cache access and refill regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

static void cache_start(void)
{
  const u32 words[] = {0x1000FFFF, 0};
  start_cpu(CPU_EXECUTION_MODE_INTERPRETER, 0xA0010000, words, 2);
}

static void cache_mode(bool tag)
{
  cpu_set_cache_control_bits(tag ? 0x1E90C : 0x1E908);
  g_cpu_state.cop0_regs.sr.Isc = 1;
  cpu_update_memory_pointers();
}

static u32 cache_load(u32 address, memory_access_size_t size)
{
  return cpu_get_memory_read_handler(address, size)(address);
}

static void cache_store(u32 address, u32 value)
{
  cpu_get_memory_write_handler(address, MEMORY_ACCESS_SIZE_WORD)(address, value);
}

TEST(CacheBiu, tag_words_select_metadata_without_changing_code)
{
  cache_start();
  const u32 address = 0x31800;
  const u32 line    = cpu_get_icache_line(address);
  const u32 code[]  = {0x11223344, 0x55667788, 0x99AABBCC, 0xDDEEFF00};
  memcpy(g_cpu_state.icache_data + line * 4, code, sizeof(code));
  cache_mode(true);
  for (u32 valid = 0; valid < 16; valid++) {
    cache_store(address, 0xF0000000 | valid);
    for (u32 word = 0; word < 4; word++) {
      EXPECT_EQ(cache_load(address + word * 4, MEMORY_ACCESS_SIZE_WORD) & 0x1F, valid | 0x10);
      EXPECT_EQ(cache_load((address | 0x80000000) + word * 4, MEMORY_ACCESS_SIZE_WORD) & 0x1F, valid | 0x10);
      EXPECT_EQ(cache_load(address + 0x1000 + word * 4, MEMORY_ACCESS_SIZE_WORD) & 0x1F, valid);
    }
    EXPECT_EQ(memcmp(g_cpu_state.icache_data + line * 4, code, sizeof(code)), 0);
  }
  cache_store(address + 0x1000, 0xF0000005);
  EXPECT_EQ(cache_load(address, MEMORY_ACCESS_SIZE_WORD) & 0x1F, 5);
  EXPECT_EQ(cache_load(address + 0x1000, MEMORY_ACCESS_SIZE_WORD) & 0x1F, 0x15);
  EXPECT_EQ(memcmp(g_cpu_state.icache_data + line * 4, code, sizeof(code)), 0);
  finish_cpu();
}

TEST(CacheBiu, tag_flush_retains_every_code_word)
{
  cache_start();
  for (u32 i = 0; i < CPU_ICACHE_SLOTS; i++)
    g_cpu_state.icache_data[i] = 0xABCD0000 | i;
  cache_mode(true);
  for (u32 line = 0; line < CPU_ICACHE_LINES; line++)
    cache_store(line * 16, 0);
  for (u32 line = 0; line < CPU_ICACHE_LINES; line++) {
    EXPECT_EQ(cache_load(line * 16, MEMORY_ACCESS_SIZE_WORD) & 0x1F, 0x10);
    for (u32 word = 0; word < 4; word++)
      EXPECT_EQ(g_cpu_state.icache_data[line * 4 + word], 0xABCD0000 | (line * 4 + word));
  }
  finish_cpu();
}

TEST(CacheBiu, code_reads_obey_width_at_cache_end)
{
  cache_start();
  cache_mode(false);
  cache_store(0xFFC, 0x89ABCDEF);
  EXPECT_EQ(cache_load(0xFFC, MEMORY_ACCESS_SIZE_WORD), 0x89ABCDEF);
  EXPECT_EQ(cache_load(0xFFC, MEMORY_ACCESS_SIZE_HALFWORD), 0xCDEF);
  EXPECT_EQ(cache_load(0xFFE, MEMORY_ACCESS_SIZE_HALFWORD), 0x89AB);
  const u32 bytes[] = {0xEF, 0xCD, 0xAB, 0x89};
  for (u32 i = 0; i < 4; i++)
    EXPECT_EQ(cache_load(0xFFC + i, MEMORY_ACCESS_SIZE_BYTE), bytes[i]);
  finish_cpu();
}

static void check_fills(bool two_words)
{
  const u32 address   = 0x31800;
  const u32 data[]    = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  const u32 invalid[] = {two_words ? 0xC : 0, 1, 3, 7};
  for (u32 offset = 0; offset < 4; offset++) {
    cache_start();
    cpu_set_cache_control_bits(two_words ? 0x1E888 : 0x1E988);
    memcpy(bus_get_unprotected_ram_pointer() + address, data, sizeof(data));
    const u32 line = cpu_get_icache_line(address);
    for (u32 i = 0; i < 4; i++)
      g_cpu_state.icache_data[line * 4 + i] = 0xDEADBEEF;
    u32 value;
    EXPECT_TRUE(cpu_fill_icache(0x80000000 | (address + offset * 4), &value));
    EXPECT_EQ(value, data[offset]);
    EXPECT_EQ(g_cpu_state.icache_tags[line], address | invalid[offset]);
    for (u32 i = 0; i < 4; i++) {
      const bool loaded = i >= offset && !(two_words && offset == 0 && i >= 2);
      EXPECT_EQ(g_cpu_state.icache_data[line * 4 + i], loaded ? data[i] : 0xDEADBEEF);
    }
    finish_cpu();
  }
}

TEST(CacheBiu, four_word_fill_offsets)
{
  check_fills(false);
}
TEST(CacheBiu, two_word_fill_offsets)
{
  check_fills(true);
}

TEST(CacheBiu, matching_refills_compose_and_replace_fetched_words)
{
  cache_start();
  const u32 address = 0x31800, line = cpu_get_icache_line(address);
  u32       data[] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
  memcpy(bus_get_unprotected_ram_pointer() + address, data, sizeof(data));
  cpu_set_cache_control_bits(0x1E888);
  u32 value;
  EXPECT_TRUE(cpu_fill_icache(address, &value));
  EXPECT_EQ(g_cpu_state.icache_tags[line], address | 0xC);
  data[0] = 0x55555555;
  data[1] = 0x66666666;
  memcpy(bus_get_unprotected_ram_pointer() + address, data, sizeof(data));
  EXPECT_TRUE(cpu_fill_icache(address + 8, &value));
  EXPECT_EQ(g_cpu_state.icache_tags[line], address);
  EXPECT_EQ(g_cpu_state.icache_data[line * 4], 0x11111111);
  EXPECT_EQ(g_cpu_state.icache_data[line * 4 + 1], 0x22222222);
  cache_mode(true);
  cache_store(address, 0xC);
  cpu_set_cache_control_bits(0x1E988);
  EXPECT_TRUE(cpu_fill_icache(address, &value));
  EXPECT_EQ(g_cpu_state.icache_tags[line], address);
  EXPECT_EQ(memcmp(g_cpu_state.icache_data + line * 4, data, sizeof(data)), 0);
  finish_cpu();
}

TEST(CacheBiu, two_word_mismatch_evicts_validity_and_keeps_unfetched_code)
{
  cache_start();
  const u32 address = 0x31800, other = address + 0x1000, line = cpu_get_icache_line(address);
  const u32 first[] = {1, 2, 3, 4}, second[] = {5, 6, 7, 8};
  memcpy(bus_get_unprotected_ram_pointer() + address, first, sizeof(first));
  memcpy(bus_get_unprotected_ram_pointer() + other, second, sizeof(second));
  cpu_set_cache_control_bits(0x1E988);
  u32 value;
  EXPECT_TRUE(cpu_fill_icache(address, &value));
  cpu_set_cache_control_bits(0x1E888);
  EXPECT_TRUE(cpu_fill_icache(other, &value));
  EXPECT_EQ(value, 5);
  EXPECT_EQ(g_cpu_state.icache_tags[line], other | 0xC);
  EXPECT_EQ(g_cpu_state.icache_data[line * 4 + 2], 3);
  EXPECT_EQ(g_cpu_state.icache_data[line * 4 + 3], 4);
  finish_cpu();
}

TEST(CacheBiu, isolated_cache_state_round_trip)
{
  cache_start();
  cache_mode(false);
  cache_store(0x31800, 0x12345678);
  cache_mode(true);
  cache_store(0x31800, 5);
  u8              buffer[16384];
  state_wrapper_t writer, reader;
  state_wrapper_init_write(&writer, buffer, sizeof(buffer), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&writer));
  EXPECT_FALSE(state_wrapper_has_error(&writer));
  cpu_clear_icache();
  cpu_set_cache_control_bits(0);
  state_wrapper_init_read(&reader, buffer, state_wrapper_get_position(&writer), SAVE_STATE_VERSION);
  EXPECT_TRUE(cpu_do_state(&reader));
  EXPECT_EQ(cpu_get_cache_control_bits(), 0x1E90C);
  EXPECT_EQ(cache_load(0x31800, MEMORY_ACCESS_SIZE_WORD) & 0x1F, 0x15);
  cache_mode(false);
  EXPECT_EQ(cache_load(0x31800, MEMORY_ACCESS_SIZE_WORD), 0x12345678);
  finish_cpu();
}

static void check_mode_program(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking)
{
  const u32 words[] = {0xAD090000, 0x8D0B0000, 0,          0x3170001F, 0xADAC0000, 0x8D0E0000,
                       0,          0x01C08821, 0xAD0F0004, 0x8D120004, 0,          0xADBA0000,
                       0xAD090000, 0x8D130000, 0,          0x3273001F, 0x1000FFFF, 0};
  for (u32 swap = 0; swap < 2; swap++) {
    start_cpu_with_fastmem(mode, fastmem, 0xA0010000, words, sizeof(words) / sizeof(words[0]));
    g_settings.cpu_recompiler_block_linking = linking;
    cache_mode(true);
    g_cpu_state.cop0_regs.sr.Swc = swap;
    cpu_update_memory_pointers();
    g_cpu_state.regs.r[8]             = 0x31800;
    g_cpu_state.regs.r[9]             = 0xA;
    g_cpu_state.regs.r[12]            = 0x1E908;
    g_cpu_state.regs.r[13]            = 0xFFFE0130;
    g_cpu_state.regs.r[15]            = 0x89ABCDEF;
    g_cpu_state.regs.r[26]            = 0x1E90C;
    const u32 line                    = cpu_get_icache_line(0x31800);
    g_cpu_state.icache_data[line * 4] = 0x12345678;
    const u32 canary                  = 0xAABBCCDD;
    memcpy(bus_get_unprotected_ram_pointer() + 0x31800, &canary, sizeof(canary));
    run_slice(1000);
    EXPECT_EQ(g_cpu_state.regs.r[16], 0x1A);
    EXPECT_EQ(g_cpu_state.regs.r[17], 0x12345678);
    EXPECT_EQ(g_cpu_state.regs.r[18], 0x89ABCDEF);
    EXPECT_EQ(g_cpu_state.regs.r[19], 0x1A);
    EXPECT_EQ(g_cpu_state.icache_data[line * 4], 0x12345678);
    u32 actual;
    memcpy(&actual, bus_get_unprotected_ram_pointer() + 0x31800, sizeof(actual));
    EXPECT_EQ(actual, canary);
    if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
      EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed >= 10);
      EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
    }
    if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
    finish_cpu();
  }
}

TEST(CacheBiu, mode_switch_interpreter)
{
  check_mode_program(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(CacheBiu, mode_switch_cached)
{
  check_mode_program(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(CacheBiu, mode_switch_native)
{
  check_mode_program(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true);
}
TEST(CacheBiu, mode_switch_mmap)
{
  check_mode_program(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true);
}
TEST(CacheBiu, mode_switch_unlinked)
{
  check_mode_program(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false);
}
