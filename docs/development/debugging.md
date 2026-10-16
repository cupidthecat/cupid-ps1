# Logging and debugging

Log levels (`src/common/log.h`): `NONE`, `ERROR`, `WARNING`, `INFO` (default), `VERBOSE`, `DEV`, `DEBUG`, `TRACE`.

Setting `CUPID_TRACE=1` raises the level to `DEV` and turns on a sampled (every 64th frame) state dump at present time, including CPU snapshots, GPU op tallies, IRQ status, DMA counts, timers, CDROM state, SPU activity. Per-subsystem ringbuffers (CPU branch log, GPU GP0/GP1, CDROM commands, SPU events) are flushed to stderr at exit when traces are enabled.

`CUPID_TRACE_PC` and `CUPID_TRACE_TASK` add CPU-PC and BIOS-task overlays. `CUPID_TRACE_CD` enables an independent CDROM command trace.

## BIOS console output

To capture BIOS console calls, enable this setting in `settings.ini`:

```ini
[BIOS]
TTYLogging = true
```

Ordinary, cached and native execution observe the A0/B0 entry points and their
cached and uncached mirrors. The BIOS instructions continue to run. Observation
uses safe memory reads, so a text pointer into device registers cannot trigger
an MMIO read. The setting also applies to native blocks compiled before it
changes.

Character calls with a file descriptor and counted writes log only stdout.
Counted writes use their byte count; `puts` observation stops at a zero byte,
an inaccessible address or 1,024 bytes. Text is buffered until a newline.

When checking a test program, require its expected result lines as well as
normal completion. Reaching a frame checkpoint without those lines leaves
the test result unverified.
