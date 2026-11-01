# Partial stores

`SWL` and `SWR` put a shifted source word and byte enables on the CPU bus.
RAM preserves the other bytes. Internal 32-bit register latches instead
accept the whole shifted word. The old register contents do not contribute.
See the [published partial-write observations](https://psx-spx.consoledev.net/ps1/system/partialwordwrites/),
which report measurements on one SCPH-5501.

The bus identifies internal word latches through the active write handler
and register region. Isolated cache mappings therefore keep their own path.
These stores reach the write handler without an initial MMIO read.

## Byte devices and serial registers

The external byte path sends the enabled source bytes in lane order. With
address stepping disabled, those writes repeat the address of the first
enabled byte. A three-byte mask therefore supplies three byte strobes on
this path. The separate 16-bit dispatch below retains its measured
halfword rules.

Serial writes select the halfword containing the first enabled byte and
use that halfword of the shifted source. For example, `SWR +2` at the mode
register reaches the control register at `+2`; it does not read and rewrite
the mode register. A store to the data register does not perform an
implicit receive-data read.

One-byte partial stores to the CD registers retain a queued response, as
equivalent byte stores do. Generated cases check both stepping settings and restored
responses across the execution paths. A held-bus two-byte store also supplies
both Setfilter parameters, checked through the five-byte Getparam response.

## SPU with a 16-bit bus

A full four-byte store issues two halfword writes. Other partial stores
issue one halfword at the lowest enabled byte. Multi-byte partial stores
assert both external write enables; single-byte partial stores retain the
addressed lane. The SPU latches the halfword when the low write enable is
active and ignores a high-only write enable.

This drops the extra bytes of a three-byte store. For example, with source
`AABBCCDD`, `SWL +2` writes `BBCC` to the left volume register and leaves its
neighbor unchanged. A full word writes `CCDD` followed by `AABB`. With address
stepping disabled, both halves reach the starting register.

## Timer registers

Timer stores pass the shifted bus value directly to the register write handler.
Target registers retain its low 16 bits. Reading a timer mode register clears
the reached-target and overflow flags, so an implicit read before a store
would consume status that the mode write preserves.

The timer regressions cover all three counters. Guest programs load source and
baseline patterns from RAM and read back each result once. Read-handler counts
must equal those explicit readbacks. Separate cases check reached flags before
and after restoring CPU, bus, timer and scheduler state.

## Execution and checks

The interpreter, cached interpreter and native helpers share the store
dispatch. Native RAM stores retain their direct host merge and write-protection
recovery. MMIO stores use the device helpers. The dispatch functions add no
state. Timer phase state is described in
[root counters](../timers/root-counters.md#save-states).

Run the generated cases with:

```sh
make test TEST_FILTER=CpuPartial
```

The cases vary source values, register baselines and byte offsets across CPU
modes, slow memory, mmap, LUT and linking settings. SPU programs cover both
stepping settings, constant and dynamic addresses, neighboring registers and
component-state restoration. Read-handler counters check that the stores add
no device word reads. The existing RAM merge, LUT and fault-recovery tests
check the other access paths.

Additional cases check all serial store offsets, register baselines, pending
loads and restored state. RAM, scratchpad and isolated instruction-cache data
mappings retain disabled bytes. The isolated cases access a hardware virtual address
through the active cache mapping, so device dispatch must not override it.
These storage cases protect the existing software byte-merge model; they
are not a physical cache-write measurement.
The generated live guest checks 96 serial captures and CD protocol effects
on Linux and native Windows, with complete restored-state comparisons.

## Remaining paths

The eight-bit multi-strobe model has component and protocol coverage; its
physical waveforms have not been captured. Byte-only device models retain
their existing mapping when configured for a 16-bit bus. Those configurations,
serial data-register aliases, isolated-cache partial stores, cache-control
partial writes and physical bus timing remain unqualified. The separate
cache-control mapping keeps its previous merge path.
The published matrix directly measures IMASK, DPCR and SPU volume registers.
The timer cases are software regressions; timer-specific partial-store hardware
measurements remain missing. The SPU measurements do not establish 8-bit device
behavior or behavior on every console revision. No new physical capture was
available.
