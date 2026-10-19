# Save states

Path conventions:

| Slot kind | Path |
|-----------|------|
| Per-game | `savestates/<serial>_<slot>.sav` |
| Global | `savestates/savestate_<slot>.sav` |

F11/F12 save and load the **active slot, global**. The active slot defaults to `1`; press `Shift+1` ... `Shift+9` to switch it (current selection is logged to stderr).

Programmatic API: `system_save_state(path, error, backup_existing, ignore_memcard_busy)` and `system_load_state(path, error, save_undo, force_update)`.

State streams support **uncompressed**, **deflate**, and **zstd** compression. Format is versioned via `SAVE_STATE_VERSION` in `src/core/state/save_state_version.h`; load paths reject unknown versions or unsupported compression types with an error.

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
