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

## Execution and checks

The interpreter, cached interpreter and native helpers share the store
dispatch. Native RAM stores retain their direct host merge and write-protection
recovery. MMIO stores use the device helpers. The save-state layout remains
version 87; the dispatch functions add no state.

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

Partial stores to 8-bit external devices and the serial/timer paths still
use a word read and merge. Their strobes and read side effects need further
work. Isolated-cache partial stores and bus timing also remain unqualified.
The published SPU measurements do not establish 8-bit device behavior or
behavior on every console revision. No new physical capture was available.
