#include "tests/core/mdec/mdec_input_fixture.h"

static void check_dma_input(bool restore)
{
  const u32 rooms[]      = {0, 1, 2, 3, 4, 63, 64, 65, 66, 67, 511, 512};
  const u32 requests[]   = {0, 1, 33};
  const u32 patterns[]   = {0xCAFE1357, 0x89ABCDEF};
  u32       observations = 0, failures = 0;
  for (u32 old = 0; old < 2; old++) {
    for (u32 room_index = 0; room_index < sizeof(rooms) / sizeof(rooms[0]); room_index++) {
      for (u32 request_index = 0; request_index < sizeof(requests) / sizeof(requests[0]); request_index++) {
        for (u32 pattern = 0; pattern < 2; pattern++) {
          const u32 program[] = {0x1000FFFF, 0};
          const u32 room      = rooms[room_index];
          begin_input(old != 0, room, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, program, 2);
          if (restore) restore_input();
          const input_snapshot_t before = read_input();
          EXPECT_EQ(before.size, 512 - room);
          u32 words[33];
          for (u32 i = 0; i < 33; i++)
            words[i] = patterns[pattern] ^ (i << 16) ^ i;
          const u32 requested = requests[request_index];
          const u32 accepted  = requested < room / 2 ? requested : room / 2;
          mdec_dma_write(words, requested);
          const input_snapshot_t after = read_input();
          bool                   equal = after.head == before.head && after.size == before.size + 2 * accepted &&
                       after.tail == (before.tail + 2 * accepted) % 512;
          for (u32 i = 0; i < before.size; i++)
            equal &= after.halfwords[(before.head + i) % 512] == before.halfwords[(before.head + i) % 512];
          for (u32 i = 0; i < 2 * accepted; i++) {
            const u16 expected = (u16)(words[i / 2] >> (16 * (i & 1)));
            equal &= after.halfwords[(before.tail + i) % 512] == expected;
          }
          printf("mdec-input-dma restore=%u old=%u room=%u requested=%u pattern=%u before=%u/%u/%u after=%u/%u/%u "
                 "expected_size=%u status=%08X/%08X complete_words=%u\n",
                 restore, old, room, requested, pattern, before.head, before.tail, before.size, after.head, after.tail,
                 after.size, before.size + 2 * accepted, before.status, after.status, equal);
          failures += !equal;
          observations++;
          end_input();
        }
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 144);
  EXPECT_EQ(failures, 0);
}

TEST(MdecInputDma, fresh)
{
  check_dma_input(false);
}
TEST(MdecInputDma, restored)
{
  check_dma_input(true);
}

static void check_mmio_input(bool restore)
{
  const u32 rooms[]      = {0, 1, 2, 3, 4, 65, 66, 67};
  const u32 patterns[]   = {0xCAFE1357, 0x89ABCDEF};
  const u32 program[]    = {0x1000FFFF, 0};
  u32       observations = 0, failures = 0;
  for (u32 old = 0; old < 2; old++) {
    for (u32 index = 0; index < sizeof(rooms) / sizeof(rooms[0]); index++) {
      for (u32 pattern = 0; pattern < 2; pattern++) {
        begin_input(old != 0, rooms[index], CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, program, 2);
        if (restore) restore_input();
        const input_snapshot_t before = read_input();
        const u32              word   = patterns[pattern];
        mdec_write_register(0, word);
        const input_snapshot_t after    = read_input_raw();
        const u32              accepted = rooms[index] >= 2 ? 1 : 0;
        const bool             equal    = input_matches(&before, &after, &word, accepted);
        printf("mdec-input-mmio restore=%u old=%u room=%u pattern=%u before=%u/%u/%u after=%u/%u/%u expected_size=%u "
               "status=%08X/%08X complete_words=%u\n",
               restore, old, rooms[index], pattern, before.head, before.tail, before.size, after.head, after.tail,
               after.size, before.size + 2 * accepted, before.status, after.status, equal);
        failures += !equal;
        observations++;
        end_input();
      }
    }
  }
  fflush(stdout);
  EXPECT_EQ(observations, 32);
  EXPECT_EQ(failures, 0);
}

TEST(MdecInputMmio, fresh)
{
  check_mmio_input(false);
}
TEST(MdecInputMmio, restored)
{
  check_mmio_input(true);
}
