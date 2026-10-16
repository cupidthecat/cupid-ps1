# Logging and debugging

Log levels (`src/common/log.h`): `NONE`, `ERROR`, `WARNING`, `INFO` (default), `VERBOSE`, `DEV`, `DEBUG`, `TRACE`.

Setting `CUPID_TRACE=1` raises the level to `DEV` and turns on a sampled (every 64th frame) state dump at present time, including CPU snapshots, GPU op tallies, IRQ status, DMA counts, timers, CDROM state, SPU activity. Per-subsystem ringbuffers (CPU branch log, GPU GP0/GP1, CDROM commands, SPU events) are flushed to stderr at exit when traces are enabled.

`CUPID_TRACE_PC` and `CUPID_TRACE_TASK` add CPU-PC and BIOS-task overlays. `CUPID_TRACE_CD` enables an independent CDROM command trace.
