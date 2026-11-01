# Device bus accesses

The external-device controls select transfer width with bit 12 and address
stepping with bit 13. An access wider than the selected width makes several
strobes. With stepping disabled, each strobe uses the starting address.
Repeated addresses still access the device, so a FIFO can advance on each
strobe. See the [memory-control specification](https://psx-spx.consoledev.net/dev/ps1/system/memorycontrol/).

## Access paths

`src/core/memory/device_bus.c` splits CPU accesses into byte or halfword
units and packs read results in little-endian order. Device callbacks apply
register behavior and write enables. CPU alignment checks happen before
these bus handlers.

| Device | Implemented data behavior |
|--------|---------------------------|
| BIOS ROM | In 8-bit mode, reads and instruction fetches either step through ROM bytes or repeat the starting byte. |
| SPU | Both configured widths use the SPU's halfword registers. Address stepping controls which register receives each unit. |
| CD-ROM | In 8-bit mode, every byte reads or writes the selected register, with address stepping applied between strobes. |
| EXP1 | Byte strobes reach the expansion interface. No device is attached; reads return `FFh` and writes have no effect. |
| EXP2 | Byte strobes reach the existing status and output handlers, with address stepping applied between them. |

In 16-bit SPU mode, an even byte write latches the source halfword; an odd
byte write has no effect. In 8-bit mode, each strobe writes `FFh` as the high
byte and the transferred byte as the low byte. The SPU ignores address bit 0,
so adjacent byte writes can overwrite the same register.

BIOS data reads and instruction decoding share the bus read function.
Instruction-cache contents and retained prefetches keep their existing
semantics. Changing a device control affects subsequent accesses; it does
not rewrite an instruction already held by the CPU.
Host block metadata records the ROM transfer view. Switching views does not
count as repeated code mutation within one view; the recompile limit itself
is unchanged.

System reset initializes the bus controls before the CPU fetches its first
instruction from the reset vector.

The serialized memory-control registers retain their 32-bit layout. The
transfer helper adds no state. Timer phase state is described in
[root counters](../timers/root-counters.md#save-states).

## Checks and limits

Run the generated regressions with:

```sh
make test TEST_FILTER=DeviceBus
```

The tests check transfer addresses, byte enables, repeated FIFO strobes,
BIOS data and fetches, SPU register writes, and CD-ROM command responses and
parameter writes. Guest programs change controls during execution under the
interpreter, cached interpreter and native backend. Native cases cover slow,
mmap and LUT accesses with linking enabled and disabled. Component-state
checks restore CPU, bus and affected peripheral state before continuing.
These tests use generated code and data; they need no private BIOS or disc.

Selecting 16-bit mode for the byte-only BIOS, CD-ROM or expansion models
retains their previous data mapping. Those configurations remain unqualified.
Fixed expansion read values do not reproduce an electrical floating bus.
Expansion instruction fetches, device-window size checks, DMA strobes and
partial-word write enables also need further work.
See [partial stores](partial-stores.md) for the supported internal register
latches and 16-bit SPU dispatch.

## Expansion read delays

EXP2 reads use the read-delay field of `1F80101Ch` and the shared periods in
`1F801020h`. The bus timing calculation sums the first access and the
sequential accesses required by the configured bus width. Address stepping
changes the selected registers without changing the number of strobes.
The CPU charges its instruction cycle separately from the bus delay.
Writes retain their existing posted behavior.

Changing either timing register recalculates the delays. Reset initializes
them from the reset controls. Loading a state rebuilds derived access delays
from its saved controls, including older records with zero EXP2 delays.
The serialized layout and state version are unchanged.

`make test TEST_FILTER=ExpansionTiming` checks all three CPU address aliases,
byte/halfword/word reads, bus widths, read versus write delay fields, common
period changes, reset and restoration of an older timing table. Generated
guest programs check delay changes across ordinary, cached and native
execution, with slow memory, mmap, LUT and disabled linking. Native cases
require execution progress through the selected backend.

These checks establish control-dependent charging in the current timing
model. The original access-time program still differs from supplied console
observations, including EXP2. Its loop overhead, CPU stalls and physical
strobe timing remain unresolved. Data loss caused by insufficient recovery
time is also unqualified. No new physical-console capture was available.
