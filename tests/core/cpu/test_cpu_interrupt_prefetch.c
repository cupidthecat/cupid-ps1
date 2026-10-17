/* cupid-ps1 interrupt, instruction prefetch and branch-slot regressions. */
#include "tests/core/cpu/cpu_execution_fixture.h"

typedef enum { IRQ_PLAIN, IRQ_TAKEN_SLOT, IRQ_UNTAKEN_SLOT } irq_position_t;

static void check_interrupt_prefetch(cpu_execution_mode_t mode, cpu_fastmem_mode_t fastmem, bool linking,
                                     irq_position_t position)
{
  for (u32 cached = 0; cached < 2; cached++) {
    for (u32 command = 0; command < 2; command++) {
      for (u32 changed = 0; changed < 2; changed++) {
        for (u32 restore = 0; restore < 2; restore++) {
          const u32  base        = cached ? 0x80010000 : 0xA0010000;
          const u32  original    = command ? 0x4A000006 : 0; /* NCLIP or NOP. */
          const u32  replacement = changed ? (command ? 0 : 0x4A000006) : original;
          const bool slot        = position != IRQ_PLAIN;
          const bool taken       = position == IRQ_TAKEN_SLOT;
          const u32  pc          = base + (slot ? 24 : 20);
          const u32  destination = base + (slot ? (taken ? 44 : 28) : 24);
          u32        words[]     = {0x24091357, 0xAD090000, 0x8D0A0000, 0, 0x26100001, original,   0x26310001,
                                    0x1000FFFF, 0,          0,          0, 0x26310001, 0x1000FFFF, 0};
          if (slot) {
            words[5] = taken ? 0x10000005 : 0x14000005;
            words[6] = original;
          }
          start_cpu_with_fastmem(mode, fastmem, base, words, sizeof(words) / sizeof(words[0]));
          g_settings.cpu_recompiler_block_linking = linking;
          g_cpu_state.regs.r[8]                   = 0xA0030000;
          const u32 handler[]                     = {0x1000FFFF, 0};
          memcpy(bus_get_unprotected_ram_pointer() + 0x80, handler, sizeof(handler));
          run_to_pc(pc);
          EXPECT_EQ(g_cpu_state.regs.r[16], 1);
          EXPECT_EQ(g_cpu_state.regs.r[10], 0x1357);
          EXPECT_EQ(g_cpu_state.next_instruction.bits, original);
          EXPECT_EQ(g_cpu_state.next_instruction_is_branch_delay_slot, slot);
          EXPECT_EQ(g_cpu_state.branch_was_taken, taken);
          EXPECT_EQ(g_cpu_state.npc, destination);
          if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
            EXPECT_TRUE(cpu_execution_stats_get()->native_blocks_entered > 0);
            EXPECT_EQ(cpu_execution_stats_get()->interpreted_instructions, 0);
            if (fastmem == CPU_FASTMEM_MODE_LUT) {
              EXPECT_TRUE(cpu_execution_stats_get()->native_lut_loads > 0);
              EXPECT_TRUE(cpu_execution_stats_get()->native_lut_stores > 0);
            }
          }
          memcpy(bus_get_unprotected_ram_pointer() + (pc & bus_get_ram_mask()), &replacement, sizeof(replacement));
          g_cpu_state.cop0_regs.sr.BEV = 0;
          g_cpu_state.cop0_regs.sr.CE2 = 1;
          g_cpu_state.cop0_regs.TAR    = 0xFEEDC0DE;
          g_cpu_state.gte_regs.r32[12] = 0;
          g_cpu_state.gte_regs.r32[13] = 2;
          g_cpu_state.gte_regs.r32[14] = 2u << 16;
          g_cpu_state.gte_regs.r32[24] = 0x12345678;
          g_cpu_state.load_delay_reg   = CPU_REG_T3;
          g_cpu_state.load_delay_value = 42;
          if (restore) {
            u8              buffer[16384];
            state_wrapper_t state;
            state_wrapper_init_write(&state, buffer, sizeof(buffer), SAVE_STATE_VERSION);
            EXPECT_TRUE(cpu_do_state(&state));
            EXPECT_FALSE(state_wrapper_has_error(&state));
            const size_t size = state_wrapper_get_position(&state);
            cpu_set_pc(pc + 0x10000);
            state_wrapper_init_read(&state, buffer, size, SAVE_STATE_VERSION);
            EXPECT_TRUE(cpu_do_state(&state));
            EXPECT_FALSE(state_wrapper_has_error(&state));
            EXPECT_EQ(state_wrapper_get_position(&state), size);
            cpu_code_cache_reset();
          }
          u32 fetched = ~original;
          EXPECT_TRUE(cpu_get_prefetched_instruction(pc, &fetched));
          EXPECT_EQ(fetched, original);
          EXPECT_EQ(g_cpu_state.npc, destination);
          const u64      before = cpu_execution_stats_get()->native_blocks_completed;
          timing_event_t stop;
          timing_event_init(&stop, "interrupt-prefetch-stop", 15, 1000, 1000, stop_cpu, NULL);
          timing_event_activate(&stop);
          g_cpu_state.cop0_regs.sr.bits |= 0x101u;
          g_cpu_state.cop0_regs.cause.bits |= 0x100u;
          cpu_check_for_pending_interrupt();
          cpu_execute();
          const u32              mac0   = g_cpu_state.gte_regs.r32[24];
          const cpu_cop0_cause_t cause  = g_cpu_state.cop0_regs.cause;
          const u32              epc    = g_cpu_state.cop0_regs.EPC;
          const u32              tar    = g_cpu_state.cop0_regs.TAR;
          const u32              load   = g_cpu_state.regs.r[11];
          const u32              prefix = g_cpu_state.regs.r[16];
          const u32              canary = g_cpu_state.regs.r[17];
          u32                    data;
          memcpy(&data, bus_get_unprotected_ram_pointer() + 0x30000, sizeof(data));
          const cpu_execution_stats_t stats = *cpu_execution_stats_get();
          timing_event_deactivate(&stop);
          timing_event_destroy(&stop);
          finish_cpu();
          printf("irq-prefetch-cell mode=%u fastmem=%u linking=%u position=%u cached=%u command=%u changed=%u "
                 "restore=%u mac0=%08X cause=%08X epc=%08X tar=%08X native=%llu interpreted=%llu slots=%llu\n",
                 mode, fastmem, linking, position, cached, command, changed, restore, mac0, cause.bits, epc, tar,
                 (unsigned long long)stats.native_blocks_completed, (unsigned long long)stats.interpreted_instructions,
                 (unsigned long long)stats.native_dispatch_delay_slots);
          fflush(stdout);
          EXPECT_EQ(mac0, command ? 4 : 0x12345678);
          EXPECT_EQ(cause.Excode, CPU_EXCEPTION_INT);
          EXPECT_EQ(cause.CE, command ? 2 : 0);
          EXPECT_EQ(cause.BD, slot);
          EXPECT_EQ(cause.BT, taken);
          EXPECT_EQ(epc, slot ? pc - 4 : pc);
          EXPECT_EQ(tar, slot ? destination : 0xFEEDC0DE);
          EXPECT_EQ(load, 42);
          EXPECT_EQ(prefix, 1);
          EXPECT_EQ(canary, 0);
          EXPECT_EQ(data, 0x1357);
          if (mode == CPU_EXECUTION_MODE_RECOMPILER) {
            EXPECT_TRUE(stats.native_blocks_completed > before);
            EXPECT_EQ(stats.interpreted_instructions, 0);
            EXPECT_EQ(stats.native_dispatch_delay_slots, 0);
          }
        }
      }
    }
  }
}

#define IRQ_POSITION_CASES(name, mode, fastmem, linking)                \
  TEST(InterruptPrefetch, name##_plain)                                 \
  {                                                                     \
    check_interrupt_prefetch(mode, fastmem, linking, IRQ_PLAIN);        \
  }                                                                     \
  TEST(InterruptPrefetch, name##_taken_slot)                            \
  {                                                                     \
    check_interrupt_prefetch(mode, fastmem, linking, IRQ_TAKEN_SLOT);   \
  }                                                                     \
  TEST(InterruptPrefetch, name##_untaken_slot)                          \
  {                                                                     \
    check_interrupt_prefetch(mode, fastmem, linking, IRQ_UNTAKEN_SLOT); \
  }

IRQ_POSITION_CASES(interpreter, CPU_EXECUTION_MODE_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
IRQ_POSITION_CASES(cached, CPU_EXECUTION_MODE_CACHED_INTERPRETER, CPU_FASTMEM_MODE_DISABLED, true)
IRQ_POSITION_CASES(native_slow, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, true)
IRQ_POSITION_CASES(native_slow_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_DISABLED, false)
IRQ_POSITION_CASES(native_mmap, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, true)
IRQ_POSITION_CASES(native_mmap_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_MMAP, false)
IRQ_POSITION_CASES(native_lut, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, true)
IRQ_POSITION_CASES(native_lut_unlinked, CPU_EXECUTION_MODE_RECOMPILER, CPU_FASTMEM_MODE_LUT, false)
