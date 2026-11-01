# Chopped manual transfers

In manual mode, chopping divides a transfer into bursts of `1 << dmaWindow`
words. The first burst starts when the channel is enabled and requested.
Each incomplete burst schedules a pause of `1 << cpuWindow` CPU clocks before
the next burst can begin. The channel remains busy during the pause. See the
[DMA register specification](https://psx-spx.consoledev.net/dmachannels/).

MADR advances and the low half of BCR decreases after each burst, including
the valid prefix of a faulted burst. A zero initial word count means 65,536
words. The upper half of BCR is preserved. A completed transfer clears busy
and uses the ordinary completion IRQ rules; intermediate bursts do not set
a completion flag. Unchopped manual transfers retain their programmed
MADR and BCR. Channel 6's fixed controls do not enable chopping.

Each channel has its own pause event. Changing a device request or running
another channel cannot bypass an unexpired pause. The pause ends even when
the device stops requesting; a later request can resume the remaining data.
Stopping the channel or resetting DMA cancels that channel's pending burst.
Same-channel device callbacks cannot recursively start another transfer while
the current burst is delivering its words.
Resumed bursts schedule from their event deadlines, including only the new
burst's bus cost. Queued stall ticks from earlier bursts do not extend later
pauses when the CPU services several expired events together.

## Save states

Version 95 appends the remaining pause of each channel to the DMA record.
Loading reconstructs active events before restoring the scheduler's absolute
timestamps. A restored prefix is not transferred again. Invalid negative,
oversized or inconsistent pause records are rejected. Earlier records contain
no per-channel pause phase; loading leaves those pauses inactive and retains
the saved registers and any existing shared transfer-halt event. It cannot
reconstruct data or progress missing from an older record.

## Verification and limits

`DmaChopping` checks burst sizes and all eight pause encodings, immediate
first-word effects, request edges, another channel's bus use, stop/reset,
zero counts, faulted prefixes, GPU readback, legacy records and restoration
during a partially elapsed pause. Accumulated CPU work is compared with
single-tick event service. A generated guest transfers 65,536 words
across frame boundaries in every CPU/memory/linking configuration and checks
exact continuation after reset and restoration.

This models burst progress and pause ownership within the existing DMA bus
tick model. It does not establish physical arbitration, CPU write-queue
overlap, transfer-start overhead or exact per-word visibility within a burst.
The supplied chopping and linked-list timing captures remain qualification
inputs; passing register and restoration checks does not establish matching
transfer durations. Its 16-bit counter measurement adds back at most one
overflow, so long transfers can wrap more than the reported correction.
Chopping in request and linked-list modes remains unqualified. No new
physical-console capture is claimed.
