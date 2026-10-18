# Partial stores

`SWL` and `SWR` put a shifted source word and byte enables on the CPU bus.
RAM preserves the other bytes. Internal 32-bit register latches instead
accept the whole shifted word. The old register contents do not contribute.
See the [published partial-write observations](https://psx-spx.consoledev.net/ps1/system/partialwordwrites/),
which report measurements on one SCPH-5501.

The bus identifies internal word latches through the active write handler
and register region. Isolated cache mappings therefore keep their own path.
These stores reach the write handler without an initial MMIO read.

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

## Remaining paths

Partial stores to 8-bit external devices and serial registers still
use a word read and merge. Their strobes and read side effects need further
work. Isolated-cache partial stores and bus timing also remain unqualified.
The published matrix directly measures IMASK, DPCR and SPU volume registers.
The timer cases are software regressions; timer-specific partial-store hardware
measurements remain missing. The SPU measurements do not establish 8-bit device
behavior or behavior on every console revision. No new physical capture was
available.
