# SPU records and restored continuation

The SPU loader stages its fields and 512 KiB of sound RAM before replacing the
live component. Truncated or invalid records leave the voices, histories,
queued transfers, sound RAM and SPU event links unchanged. The reader reports
its sticky error; it does not resume a sweep or reschedule an event after
rejection. Writing a record does not allocate a staging buffer.

The wire layout remains version 97. Successful loads retain the recorded
values, including fractional clocks, decoded samples, reverb rings and FIFO
contents. The existing sweep compatibility repair runs after the staged
record has been committed.

## Validation

These are bounds of the current component model. They prevent an accepted
record from indexing beyond its buffers or starting with an invalid phase.

| Field | Accepted range |
| --- | --- |
| Reverb resampling position | 0 through 63 |
| Transfer byte address | Even addresses from 0 through 524,286 |
| Voice sample index | 0 through 27 |
| Voice envelope phase | Off through release, encoded as 0 through 4 |
| Fractional sample clock | Nonnegative and below the configured divider; 0 through 767 at native clock |
| Capture byte position | Even offsets from 0 through 1,022 |
| Fractional transfer clock | 0 through 15 |
| Noise divider and counter | Divider below 32,768; counter below 8 |
| FIFO | Head/tail below 32, size at most 32, and consistent circular indices |

The existing envelope checks also reject invalid raw boolean representations,
rates, counters and counter increments. A sample-clock phase incompatible with
the currently configured divider is rejected; conversion between different
clock settings is not provided here.

## Legacy records

The supported version range starts at 42. Records before version 89 omit the
fractional transfer clock; it restarts at zero. Records before version 92 omit
the noise divider and counter; those phases restart at zero while the recorded
noise level remains intact. Writing an older record preserves the live phases.

Version 90 and newer records restore the transfer event through the shared
scheduler. Older records defer reconstructing that event until
`spu_post_load_state`, after the saved global clock and CPU ticks have been
restored. Staging preserves this order.

## Evidence and limits

`SpuStateRecords` covers every supported version through 97, adjacent invalid
values and signed limits, each voice, complete and truncated sound RAM, raw
envelope fields, FIFO consistency and valid boundary values. Rejected records
must preserve complete CPU, bus, DMA, interrupt, SPU and scheduler records.
Additional continuation cases compare the subsequent audio captures and
transfer results with uninterrupted execution.

The original generated guest runs ADPCM and noise voices, sweeps, captures and
reverb in NTSC/PAL across interpretation, cached interpretation and linked or
unlinked native slow-memory, LUT and mmap execution. It requires nonzero voice
captures, live reverb history and work RAM, positive execution-path counts and
complete saved-byte equality after resetting before restoration.

The unmodified published SPU transfer program reports nine passing groups in
interpreter and native LUT runs on both hosts, with unchanged complete TTY text
before and after this correction. Its [pinned source](https://github.com/JaCzekanski/ps1-tests/blob/5f73d61b27a8d51269b54dde1c456d2d2f7dc865/spu/memory-transfer/main.cpp)
runs two immediate-DMA groups absent from the bundled seven-group sample log.
That strict sequence mismatch remains recorded. These results do not establish
an exact physical transfer phase.

This is component rollback. The whole-machine loader may already have restored
earlier components before reaching a bad SPU record. Atomic rollback of the
whole machine remains separate work. Host audio queues are outside the saved
record; these software checks do not qualify audible playback, physical SPU
timing or representative gameplay.
