/* cupid-ps1 complete component snapshots for generated CD streams. */
#ifndef CUPID_TEST_CD_CONTINUATION_FIXTURE_H
#define CUPID_TEST_CD_CONTINUATION_FIXTURE_H
#include "tests/core/cdrom/audio/xa_sector_fixture.h"
enum {
  XA_CD_CAPACITY     = 1024 * 1024,
  XA_CPU_CAPACITY    = 16384,
  XA_TIMING_CAPACITY = 8192,
  XA_SPU_CAPACITY    = SPU_RAM_SIZE + 65536
};
typedef struct {
  u8     cd[XA_CD_CAPACITY], cpu[XA_CPU_CAPACITY], irq[128], timing[XA_TIMING_CAPACITY], spu[XA_SPU_CAPACITY];
  size_t cd_size, cpu_size, irq_size, timing_size, spu_size;
} xa_saved_t;
#define XA_SAVE(member, fn)                                                                  \
  do {                                                                                       \
    state_wrapper_init_write(&sw, saved->member, sizeof(saved->member), SAVE_STATE_VERSION); \
    EXPECT_TRUE(fn(&sw));                                                                    \
    saved->member##_size = state_wrapper_get_position(&sw);                                  \
  } while (0)
#define XA_LOAD(member, fn)                                                                \
  do {                                                                                     \
    state_wrapper_init_read(&sw, saved->member, saved->member##_size, SAVE_STATE_VERSION); \
    EXPECT_TRUE(fn(&sw));                                                                  \
    EXPECT_EQ(state_wrapper_get_position(&sw), saved->member##_size);                      \
  } while (0)
static xa_saved_t* xa_save(void) {
  xa_saved_t* saved = malloc(sizeof(*saved));
  EXPECT_NOT_NULL(saved);
  state_wrapper_t sw;
  XA_SAVE(cpu, cpu_do_state);
  XA_SAVE(irq, interrupt_controller_do_state);
  XA_SAVE(cd, cdrom_do_state);
  XA_SAVE(spu, spu_do_state);
  XA_SAVE(timing, timing_events_do_state);
  return saved;
}
static void xa_restore(const xa_saved_t* saved) {
  state_wrapper_t sw;
  XA_LOAD(cpu, cpu_do_state);
  XA_LOAD(irq, interrupt_controller_do_state);
  XA_LOAD(cd, cdrom_do_state);
  XA_LOAD(timing, timing_events_do_state);
  /* The paused consumer has done no work. Restoring SPU would itself reactivate
   * it; retain the invariant consumer state and compare all its bytes instead. */
}
#define XA_COMPARE(member)                                        \
  do {                                                            \
    EXPECT_EQ(a->member##_size, b->member##_size);                \
    EXPECT_EQ(memcmp(a->member, b->member, a->member##_size), 0); \
  } while (0)
static void xa_equal(const xa_saved_t* a, const xa_saved_t* b) {
  XA_COMPARE(cpu);
  XA_COMPARE(irq);
  XA_COMPARE(cd);
  XA_COMPARE(spu);
  XA_COMPARE(timing);
}
static void xa_spu_equal(const xa_saved_t* a, const xa_saved_t* b) {
  XA_COMPARE(spu);
}

#endif
