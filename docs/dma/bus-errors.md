# DMA bus errors

DMA validates physical word addresses against the configured RAM window before
applying the backing-RAM mirror mask. The default window is 8 MiB. A transfer
that reaches an invalid address retains the RAM writes and device operations
performed before that word. Counting down past address zero reaches `FFFFFC`,
which is outside the default window. See the [DMA register specification](https://psx-spx.consoledev.net/dmachannels/).

A bus error clears the channel's busy flag and sets DICR bit 15. That bit forces
the master IRQ output regardless of the channel and master enables. A fault
does not set a channel-completion flag or increment the completed-transfer
counter. Existing completion flags remain until acknowledged. Clearing the
error through DICR releases the IRQ output; another error can assert it again.

Manual transfers retain MADR and BCR. Request transfers decrement the block
count when starting a block and retain that block's address on a fault. A linked
list reads its next pointer before transferring the payload; payload faults
retain that pointer. Header and payload addresses are checked separately, so
the last valid word can hold a readable header even when the payload would be
outside RAM. An empty node ending with `FFFFFF` completes successfully there.

GPU reads consume only the valid prefix, leaving later VRAM words available
through GP0. A faulted ordering table retains a previous-address link in its
last valid word, rather than inserting a premature terminator.

## Checks and limits

`DmaBusError` component checks cover forward and reverse limits, invalid starts,
the configured 2 MiB window, zero block/count encodings, valid final words,
request and linked-list registers, IRQ acknowledgement and GPU/DMA restoration.
The generated guest in `tests/integration/dma/` runs twelve cases through the
interpreter, cached interpreter and native paths with slow, LUT and mapped
memory, with and without native linking. It compares exact saved continuation
after resetting away a faulted transfer and an unread GPU word.

These checks establish the tested register and data behavior. They do not
qualify physical bus timing, per-word CPU arbitration, chopping or all device
backpressure interactions. There is no new physical-console boundary capture.
The state layout remains version 94.
