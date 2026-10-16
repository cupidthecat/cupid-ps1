/* BIOS output observation must preserve CPU execution and avoid device reads. */
#include "tests/core/cpu/cpu_execution_fixture.h"
#include "common/log.h"

typedef struct tty_capture {
  char   text[4096];
  size_t size;
  u32    lines;
} tty_capture_t;

static void capture_tty(void* user, u32 category, const char* function, const char* message, size_t size)
{
  (void)function;
  if (log_unpack_channel(category) != LOG_CHANNEL_TTY) return;
  tty_capture_t* capture = user;
  EXPECT_TRUE(capture->size + size + 2 <= sizeof(capture->text));
  memcpy(capture->text + capture->size, message, size);
  capture->size += size;
  capture->text[capture->size++] = '\n';
  capture->text[capture->size]   = '\0';
  capture->lines++;
}

static void observe_tty(tty_capture_t* capture, bool enabled)
{
  memset(capture, 0, sizeof(*capture));
  log_set_log_level(LOG_LEVEL_INFO);
  log_set_channel_enabled(LOG_CHANNEL_TTY, true);
  log_register_callback(capture_tty, capture);
  g_settings.bios_tty_logging = enabled;
}

static void finish_tty(tty_capture_t* capture)
{
  log_unregister_callback(capture_tty, capture);
  finish_cpu();
}

static void start_call(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, u32 segment, u32 entry, u32 function)
{
  const u32 words[] = {0x34090000 | function, 0x0C000000 | (entry >> 2), 0, 0x26100001, 0x1000FFFF, 0};
  start_cpu_with_fastmem(mode, fastmem, segment | 0x10000, words, 6);
  const u32 trampoline[] = {0x03E00008, 0}; /* jr ra; nop */
  memcpy(bus_get_unprotected_ram_pointer() + entry, trampoline, sizeof(trampoline));
  if (fastmem == CPU_FASTMEM_MODE_MMAP) EXPECT_TRUE(cpu_code_cache_is_using_fastmem());
}

static void check_strings(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem)
{
  const u32 segments[] = {0, 0x80000000, 0xA0000000};
  for (size_t segment = 0; segment < sizeof(segments) / sizeof(segments[0]); segment++) {
    for (u32 entry = 0xA0; entry <= 0xB0; entry += 0x10) {
      start_call(mode, fastmem, segments[segment], entry, entry == 0xA0 ? 0x3E : 0x3F);
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "hello\n", 7);
      g_cpu_state.regs.r[4] = segments[segment] | 0x30000;
      tty_capture_t capture;
      observe_tty(&capture, true);
      run_slice(200);
      EXPECT_STREQ(capture.text, "hello\n");
      EXPECT_EQ(capture.lines, 1);
      EXPECT_EQ(g_cpu_state.regs.r[16], 1);
      if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
        EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_completed > 0);
        EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      }
      finish_tty(&capture);
    }
  }
}

TEST(BiosTty, interpreter_and_cached_capture_entry_mirrors)
{
  check_strings(CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
  check_strings(CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(BiosTty, native_captures_entry_mirrors)
{
  check_strings(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED);
}

TEST(BiosTty, mmap_captures_entry_mirrors)
{
  check_strings(CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP);
}

TEST(BiosTty, logging_changes_without_recompilation)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    const u32 words[] = {0x3409003E, 0x0C000028, 0, 0x26100001, 0x1000FFFB, 0};
    start_cpu(modes[mode], 0x00010000, words, 6);
    const u32 trampoline[] = {0x03E00008, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0xA0, trampoline, sizeof(trampoline));
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "toggle\n", 8);
    g_cpu_state.regs.r[4] = 0x30000;
    tty_capture_t capture;
    observe_tty(&capture, false);
    run_slice(100);
    EXPECT_EQ(capture.lines, 0);
    const u32 before            = g_cpu_state.regs.r[16];
    g_settings.bios_tty_logging = true;
    run_slice(100);
    EXPECT_TRUE(capture.lines > 0);
    EXPECT_TRUE(g_cpu_state.regs.r[16] > before);
    const u32 lines             = capture.lines;
    g_settings.bios_tty_logging = false;
    run_slice(100);
    EXPECT_EQ(capture.lines, lines);
    finish_tty(&capture);
  }
}

TEST(BiosTty, in_block_observation_preserves_load_delay_and_cpu_state)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER, CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    u8     states[2][16384];
    size_t sizes[2];
    for (u32 enabled = 0; enabled < 2; enabled++) {
      /* The load at 9Ch is still pending when observation runs at A0h. */
      const u32 words[] = {0x3409003E, 0x3C040003, 0x8D040000, 0, 0xAE240000, 0x1000FFFF, 0};
      start_cpu_with_fastmem(modes[mode], mode == 3 ? CPU_FASTMEM_MODE_MMAP : CPU_FASTMEM_MODE_DISABLED, 0x94, words,
                             7);
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "old\n", 5);
      const u32 pointer = 0x1F802000;
      memcpy(bus_get_unprotected_ram_pointer() + 0x30020, &pointer, sizeof(pointer));
      g_cpu_state.regs.r[8]  = 0x30020;
      g_cpu_state.regs.r[17] = 0x30040;
      tty_capture_t capture;
      observe_tty(&capture, enabled != 0);
      run_slice(200);
      EXPECT_STREQ(capture.text, enabled ? "old\n" : "");
      EXPECT_EQ(g_cpu_state.regs.r[4], pointer);
      u32 stored;
      memcpy(&stored, bus_get_unprotected_ram_pointer() + 0x30040, sizeof(stored));
      EXPECT_EQ(stored, pointer);
      state_wrapper_t writer;
      state_wrapper_init_write(&writer, states[enabled], sizeof(states[enabled]), SAVE_STATE_VERSION);
      EXPECT_TRUE(cpu_do_state(&writer));
      EXPECT_TRUE(timing_events_do_state(&writer));
      sizes[enabled] = state_wrapper_get_position(&writer);
      EXPECT_FALSE(state_wrapper_has_error(&writer));
      finish_tty(&capture);
    }
    EXPECT_EQ(sizes[0], sizes[1]);
    EXPECT_EQ(memcmp(states[0], states[1], sizes[0]), 0);
  }
}

TEST(BiosTty, delay_slot_observes_branch_link)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    const u32 words[] = {0x03402009, 0}; /* jalr a0,k0; nop at A0h */
    start_cpu(modes[mode], 0x9C, words, 2);
    const u32 target[] = {0x26100001, 0x1000FFFF, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0x10000, target, sizeof(target));
    memcpy(bus_get_unprotected_ram_pointer() + 0xA4, "linked\n", 8);
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "old\n", 5);
    g_cpu_state.regs.r[4]  = 0x30000;
    g_cpu_state.regs.r[9]  = 0x3E;
    g_cpu_state.regs.r[26] = 0x10000;
    tty_capture_t capture;
    observe_tty(&capture, true);
    run_slice(200);
    EXPECT_STREQ(capture.text, "linked\n");
    EXPECT_EQ(capture.lines, 1);
    EXPECT_EQ(g_cpu_state.regs.r[16], 1);
    finish_tty(&capture);
  }
}

TEST(BiosTty, interpreter_fallback_does_not_duplicate_output)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    const u32 words[] = {0x42000001, 0}; /* TLBR raises RI through shared execution. */
    start_cpu(modes[mode], 0xA0, words, 2);
    const u32 handler[] = {0x1000FFFF, 0};
    memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "once\n", 6);
    g_cpu_state.regs.r[4]        = 0x30000;
    g_cpu_state.regs.r[9]        = 0x3E;
    g_cpu_state.cop0_regs.sr.BEV = 0;
    tty_capture_t capture;
    observe_tty(&capture, true);
    run_slice(200);
    EXPECT_STREQ(capture.text, "once\n");
    EXPECT_EQ(capture.lines, 1);
    EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, CPU_EXCEPTION_RI);
    EXPECT_EQ(g_cpu_state.cop0_regs.EPC, 0xA0);
    finish_tty(&capture);
  }
}

TEST(BiosTty, conditional_delay_slot_keeps_branch_outcome)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    for (u32 taken = 0; taken < 2; taken++) {
      const u32 words[] = {0x12323FD8, 0, 0x24100002, 0x1000FFFF, 0};
      start_cpu(modes[mode], 0x9C, words, 5);
      const u32 target[] = {0x24100001, 0x1000FFFF, 0};
      memcpy(bus_get_unprotected_ram_pointer() + 0x10000, target, sizeof(target));
      memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "slot\n", 6);
      g_cpu_state.regs.r[4]  = 0x30000;
      g_cpu_state.regs.r[9]  = 0x3E;
      g_cpu_state.regs.r[17] = 7;
      g_cpu_state.regs.r[18] = taken ? 7 : 8;
      tty_capture_t capture;
      observe_tty(&capture, true);
      run_slice(200);
      EXPECT_STREQ(capture.text, "slot\n");
      EXPECT_EQ(capture.lines, 1);
      EXPECT_EQ(g_cpu_state.regs.r[16], taken ? 1 : 2);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
      if (modes[mode] == CPU_EXECUTION_MODE_RECOMPILER)
        EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
      finish_tty(&capture);
    }
  }
}

TEST(BiosTty, counted_stdout_write_respects_descriptor_and_length)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    for (u32 entry = 0xA0; entry <= 0xB0; entry += 0x10) {
      for (u32 descriptor = 1; descriptor <= 2; descriptor++) {
        start_call(modes[mode], CPU_FASTMEM_MODE_DISABLED, 0, entry, entry == 0xA0 ? 3 : 0x35);
        memcpy(bus_get_unprotected_ram_pointer() + 0x30000, "short\nignored\n", 15);
        g_cpu_state.regs.r[4] = descriptor;
        g_cpu_state.regs.r[5] = 0x30000;
        g_cpu_state.regs.r[6] = 6;
        tty_capture_t capture;
        observe_tty(&capture, true);
        run_slice(200);
        EXPECT_STREQ(capture.text, descriptor == 1 ? "short\n" : "");
        finish_tty(&capture);
      }
    }
  }
}

TEST(BiosTty, counted_write_continues_after_zero_byte)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    const char payload[] = {'a', 0, 'b', '\n', 'x'};
    start_call(modes[mode], CPU_FASTMEM_MODE_DISABLED, 0, 0xA0, 3);
    memcpy(bus_get_unprotected_ram_pointer() + 0x30000, payload, sizeof(payload));
    g_cpu_state.regs.r[4] = 1;
    g_cpu_state.regs.r[5] = 0x30000;
    g_cpu_state.regs.r[6] = 4;
    tty_capture_t capture;
    observe_tty(&capture, true);
    run_slice(200);
    EXPECT_STREQ(capture.text, "ab\n");
    finish_tty(&capture);
  }
}

TEST(BiosTty, character_functions_respect_stdout_descriptor)
{
  const cpu_execution_mode_t modes[]    = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                           CPU_EXECUTION_MODE_RECOMPILER};
  const u32                  calls[][2] = {{0xA0, 0x09}, {0xA0, 0x3C}, {0xB0, 0x3B}, {0xB0, 0x3D}};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    for (size_t call = 0; call < sizeof(calls) / sizeof(calls[0]); call++) {
      for (u32 descriptor = 1; descriptor <= 2; descriptor++) {
        start_call(modes[mode], CPU_FASTMEM_MODE_DISABLED, 0, calls[call][0], calls[call][1]);
        g_cpu_state.regs.r[4] = 'c';
        g_cpu_state.regs.r[5] = descriptor;
        tty_capture_t capture;
        observe_tty(&capture, true);
        run_slice(200);
        bus_add_tty_character('\n');
        const bool has_descriptor = calls[call][1] == 0x09 || calls[call][1] == 0x3B;
        EXPECT_STREQ(capture.text, has_descriptor && descriptor != 1 ? "" : "c\n");
        finish_tty(&capture);
      }
    }
  }
}

static u32 device_reads;

static u32 read_tty_device(u32 address)
{
  (void)address;
  device_reads++;
  return 'x';
}

TEST(BiosTty, observation_does_not_read_mmio)
{
  const cpu_execution_mode_t modes[] = {CPU_EXECUTION_MODE_INTERPRETER, CPU_EXECUTION_MODE_CACHED_INTERPRETER,
                                        CPU_EXECUTION_MODE_RECOMPILER};
  for (size_t mode = 0; mode < sizeof(modes) / sizeof(modes[0]); mode++) {
    for (u32 counted = 0; counted < 2; counted++) {
      start_call(modes[mode], CPU_FASTMEM_MODE_DISABLED, 0, 0xA0, counted ? 3 : 0x3E);
      g_cpu_state.regs.r[4] = counted ? 1 : 0x1F802000;
      g_cpu_state.regs.r[5] = 0x1F802000;
      g_cpu_state.regs.r[6] = 16;
      bus_memory_read_handler_t* reads =
          bus_offset_read_handler_array(g_cpu_state.memory_handlers, MEMORY_ACCESS_SIZE_BYTE);
      const u32                       page     = 0x1F802000 >> BUS_MEMORY_LUT_PAGE_SHIFT;
      const bus_memory_read_handler_t original = reads[page];
      reads[page]                              = read_tty_device;
      device_reads                             = 0;
      tty_capture_t capture;
      observe_tty(&capture, true);
      run_slice(200);
      reads[page] = original;
      EXPECT_EQ(device_reads, 0);
      EXPECT_EQ(capture.lines, 0);
      EXPECT_EQ(g_cpu_state.cop0_regs.cause.Excode, 0);
      EXPECT_EQ(g_cpu_state.regs.r[16], 1);
      finish_tty(&capture);
    }
  }
}
