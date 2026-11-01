# Save states

Path conventions:

| Slot kind | Path |
|-----------|------|
| Per-game | `savestates/<serial>_<slot>.sav` |
| Global | `savestates/savestate_<slot>.sav` |

F11/F12 save and load the **active slot, global**. The active slot defaults to `1`; press `Shift+1` ... `Shift+9` to switch it (current selection is logged to stderr).

Programmatic API: `system_save_state(path, error, backup_existing, ignore_memcard_busy)` and `system_load_state(path, error, save_undo, force_update)`.

Frame-triggered frontend actions run after the current CPU instruction and
device callbacks complete. This includes saves, loads and diagnostic
checkpoints. A timer write can synchronize the GPU and finish a frame from
inside the instruction; deferring the load preserves the completed instruction
and callback state. Pending frame actions request CPU event service
without adding emulated cycles. This host action flag is not save-state data;
the format remains version 93.

State streams support **uncompressed**, **deflate**, and **zstd** compression. Format is versioned via `SAVE_STATE_VERSION` in `src/core/state/save_state_version.h`; load paths reject unknown versions or unsupported compression types with an error.

Version 93 records colored MDEC DMA placement and its partial-transfer cursor.
Earlier queued pixels retain their saved order until drained. See
[MDEC state records](../mdec/state-records.md) for compatibility and checks.

Version 92 records the SPU noise divider and step-counter phase. Earlier
records retain their saved noise level and restart that missing phase at zero.
See [shared noise](../spu/noise.md) for rates, continuation and qualification.

Version 91 adds the memory card's pending 128-byte write sector and SIO0's
selected device and in-flight byte, separately from its queued transmit byte. Interrupted
or rejected card writes no longer alter committed data. Earlier states retain
their already-saved card image and seed any remaining write from that sector;
they cannot recover data that an older emulator already overwrote or a shift
latch that an older format never stored. Card state
loads reject invalid protocol states, transfer cursors and read-data addresses.

Version 90 records the independent SPU transfer event. Legacy loads rebuild
its deadline after restoring the saved clock. Version 89 preserves the SPU's
fractional transfer tick balance. Earlier
states load with zero transfer credit. Invalid transfer balances and FIFO
ring records are rejected. See [SPU transfer accounting](../spu/transfer-budget.md).

Version 88 preserves each root counter's pending reset phase. Earlier states
load with no pending hold because they contain no phase field. See
[root-counter state](../timers/root-counters.md#save-states) for the layout
and continuation checks.

`--test-savestate-roundtrip` requests a save at frame 300, reloads it, runs
60 more frames, then shuts down. Completion is reported after frontend
teardown. This check requires local BIOS and game inputs; public CI does not
run them. Use the [bounded runtime runner](../development/testing.md) to
check process exit and save/load evidence. The smoke check does not prove
state equivalence.
