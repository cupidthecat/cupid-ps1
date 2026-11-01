# Forced manual transfers

In manual mode, CHCR bit 28 can start a transfer without a device request.
The controller clears bit 28 when delivery begins. Bit 29 holds a forced
transfer paused; clearing it allows delivery to continue. A transfer started
by DREQ ignores this pause bit. See the
[DMA register specification](https://psx-spx.consoledev.net/dmachannels/).

The controller retains the start cause during a chopped manual transfer.
Once a forced burst starts, later bursts do not need DREQ. Its chopping
windows still apply, and setting bit 29 can hold the remaining words after
the CPU window expires. A later request edge does not change the original
start cause. Stop, reset, completion and a RAM bus error discard that cause.

## Saved continuation

Version 96 appends one byte to the DMA record, containing the forced-origin
flags for channels 0 through 5. The existing chopped-pause fields retain their
version 95 layout. A forced transfer can resume after reset and restoration
when its trigger is already clear and DREQ is absent. Invalid reserved/OTC
bits, inconsistent origin flags and a missing mask byte are rejected.

Earlier records contain no start-cause flag. Loading retains their registers
and available pause phase but defaults to a request-driven continuation.
It cannot reconstruct a force origin absent from the record.

## Verification and limits

`DmaForced` checks trigger gating, initial and in-progress pauses, DREQ edges,
chopped progress, stop/reset/error cleanup, expired and unexpired restored
pauses, older records and invalid records. The generated guest leaves a known
value in the GPU read latch with no VRAM read packet asserting DREQ. It forces
one word, pauses across a saved frame, resets before restoring, then resumes
without a new trigger or request. All CPU execution modes and native memory
and linking configurations must produce exact saved continuation.

This covers manual-mode controller behavior. Forced starts and bit 29 in
request and linked-list modes remain unqualified. GPU direction mismatches,
physical per-word bus timing and preemption remain separate qualification
gaps. No new physical-console capture is claimed.
