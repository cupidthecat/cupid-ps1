# DMA channel priority

Ready channels are selected using all three priority bits in each DPCR field.
Lower values run first. Equal values favor the higher channel number. A
disabled master, inactive request, stopped channel or unexpired chopping pause
does not participate. These rules follow the
[DMA register specification](https://psx-spx.consoledev.net/dmachannels/#1f8010f0h-dpcr-dma-control-register-rw).

Control writes, request changes, shared transfer resumes and chopped resumes
use the same dispatcher. Device callbacks can change requests while a transfer
is delivering data. Selection waits until that transfer returns, so callbacks
cannot recursively run another channel through a partially delivered block.
The dispatcher checks priority again after each transfer returns.

Chopping pauses ending at the same timestamp become eligible together. Their
event insertion order does not determine which burst runs first. A later burst
includes the bus cost of preceding transfers in that dispatch when scheduling
its next pause. Earlier queued CPU stall ticks are not charged again.

DPCR and the existing transfer records determine restored selection. No new
save-state fields are required. Reset and load clear the transient dispatch
guard; the guard never changes the saved payload format.

## Verification and limits

`DmaPriority` tests all 64 GPU/OTC priority pairs in both directions. Competing
RAM writes expose the final writer, and competing RAM reads expose whether a
GPU interrupt command is delivered before OTC overwrites it. It also checks
master enables, shared-halt restoration and coincident GPU/SPU chopped resumes
using actual RAM, VRAM and sound RAM effects.

The generated priority guest runs both matrices through CPU MMIO. Its first
pair is saved while armed with both master enables clear. Every CPU execution
mode and native memory/linking configuration must resume exactly after reset
and restoration, then produce the expected data, busy flags and completion IRQs.

Selection occurs at existing transfer, block-slice and chopped-burst boundaries.
This does not implement priority preemption within an active block, physical
bus handoff timing, CPU memory priority or CPU write-queue overlap. The shared
slice-halt approximation remains. No physical-console collision capture or
complete DMA arbitration qualification is claimed.
