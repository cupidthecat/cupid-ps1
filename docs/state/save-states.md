# Save states

Path conventions:

| Slot kind | Path |
|-----------|------|
| Per-game | `savestates/<serial>_<slot>.sav` |
| Global | `savestates/savestate_<slot>.sav` |

F11/F12 save and load the **active slot, global**. The active slot defaults to `1`; press `Shift+1` ... `Shift+9` to switch it (current selection is logged to stderr).

Programmatic API: `system_save_state(path, error, backup_existing, ignore_memcard_busy)` and `system_load_state(path, error, save_undo, force_update)`.

State streams support **uncompressed**, **deflate**, and **zstd** compression. Format is versioned via `SAVE_STATE_VERSION` in `src/core/save_state_version.h`; load paths reject unknown versions or unsupported compression types with an error.

`--test-savestate-roundtrip` requests a save at frame 300, reloads it, then requests 60 more frames. It requires local BIOS and game inputs and is not run in public CI. Its shutdown behavior is tracked in [issue #6](https://github.com/cupidthecat/cupid-ps1/issues/6). Require process exit 0 and completed save/load and continuation evidence; a printed PASS message alone is insufficient. This smoke test does not prove state equivalence.
