# Ordering-table DMA

Channel 6 writes a reverse ordering table to RAM. Its control register has three
writable bits: start/busy (24), trigger (28) and bit 30. Address stepping is
fixed at minus four bytes; direction, sync mode, chopping and window fields
are read-only. The control register reads `00000002` after reset. See the
[DMA register specification](https://psx-spx.consoledev.net/dev/ps1/system/dmachannels/).

Both start and trigger must be set before a transfer can run. If DPCR disables
the channel, the transfer waits until it is enabled. Trigger clears at the
start and busy clears at completion. The table ends with `00FFFFFF`; a zero
word count selects 65,536 words. MADR and BCR retain their programmed values.
Completion uses the ordinary DMA interrupt enable and acknowledgement logic.

State format 94 is unchanged. Loading a record enforces the channel's fixed
control bits and derives its request from the trigger. This also corrects
older records containing unsupported direction or sync fields. Loading does
not start a pending transfer by itself or reconstruct RAM written by an older
implementation. Completed current records retain exact serialization through
reset and restoration.

Component regressions cover control readback, all direction/sync encodings,
chopping writes, trigger and master-enable gating, zero counts, mirrored RAM
boundaries, completion IRQs and state restoration. A generated guest checks
64 control combinations and exact continuation after resetting away a transfer
waiting for DPCR enable, across all CPU modes and native memory paths.

These tests do not qualify physical bus timing, bit-30 cache snooping or DMA
arbitration. Transfers crossing the physical RAM window, including address
underflow, still use the existing bus-error model and need further work.
