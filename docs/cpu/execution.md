# CPU execution

Defined in `src/core/shared/types.h`, dispatched by `cpu_execute()` in `src/core/cpu/cpu_core.c`.

| Mode | What it is |
|------|-----------|
| `interpreter` | MIPS R3000A interpreter; also used by the trace diagnostics. |
| `cached_interpreter` | Reuses instruction blocks with the shared interpreter executor. Per-instruction fetches preserve pipeline, instruction-cache and tick behavior. |
| `recompiler` | JIT to native code. Default on x86-64 hosts. The AArch64 backend uses a per-block interpreter fallback; ARM hosts are not qualified by the current build workflow. |

## Cached interpreter

The cached interpreter validates each cached word against the current
prefetched instruction and leaves the block when code, PC or event timing
changes. Generated-program tests compare CPU state, RAM and tick checkpoints
with ordinary interpreter execution, including branch/load delays, exceptions,
interrupted blocks and self-modifying code. These tests cover their generated
cases; they do not establish complete CPU accuracy.

Set `CUPID_CPU_STATS=1` before startup to print execution configuration,
interpreted instruction counts, native block entry/completion counts and
cached-block creation, reuse, executed-word and fallback counts at shutdown. The runtime smoke runner requires positive
creation, reuse and execution counts when `--mode cached` is selected.

## Execution state and timing

All three execution modes share instruction prefetch. Switching modes retains
the fetched word, pending loads, instruction-cache data and partial-line
validity. Before native dispatch starts, pending branch-delay instructions
finish through the shared executor.

Native instruction-cache emulation is enabled by default. Blocks decode the
words the CPU cache serves, including stale instructions after a RAM write.
Before each compiled instruction executes, native code checks its latched word
and event deadline, then fetches the next instruction through the shared CPU
path. Changed words invalidate host code without repeating the block prefix.
A pending branch slot retains its resolved target and branch outcome when
execution returns to the dispatcher. These exits preserve pending loads and
busy-unit deadlines.

Generated cases include slow memory, mmap fastmem and execution without block
linking. They check the shared pipeline model; full native fetch and event
timing still need hardware qualification.

GTE commands arm a completion deadline. Result-register reads wait for that
deadline, including native reads whose destination is register zero. Outstanding
GTE and multiply/divide deadlines remain in CPU save states and cross native
block boundaries. Generated cases compare complete CPU and event serializations
after uninterrupted and restored continuations. They cover those programs;
full native timing and instruction-cache behavior remain under qualification.

Native COP2 register transfers and commands check the current status register
before accessing the GTE. A disabled GTE raises the coprocessor-unusable
exception, including for reads to register zero. These instructions remain
after their branch condition when compiled in a delay slot, so the exception
retains the branch outcome and target.

All three CPU modes check the enable bits for COP1 and COP3 operations and
coprocessor memory transfers. Native COP0 register operations also check user
mode permissions. These checks use the current status register, including
changes made earlier in the same block. Disabled transfers trap before device
access or a GTE busy wait. Generated cases check pending loads and exceptions
in both taken and untaken branch slots.

Enabled LWC0/1/3 instructions read a word and discard its value in all three
modes. Native code preserves device-read effects, checks alignment and
propagates reported bus failures without interpreting the instruction. Faults
retain preceding register writes, pending loads and branch context. Generated
cases cover constant and dynamic addresses, normal and taken/untaken branch
slots, slow memory, mmap fastmem and disabled block linking. The bus-failure
cases inject an error through a valid device handler; they check propagation,
not physical address decoding or hardware timing.

Enabled SWC0/1/3 stores still need qualification.

With COP2 enabled, unsupported GTE command encodings are logged and execution
continues without a CPU exception. Their arithmetic effects and exact timing
[remain unqualified](https://github.com/cupidthecat/cupid-ps1/issues/18).

## Coprocessor branches

With the corresponding CU bit enabled, `BCnF` branches and `BCnT` falls through
for all four coprocessors. Both execute their delay slot. The `rt=2/3` encodings
behave as `rt=0/1`, without branch-likely nullification. A pending GTE command
or FLAG.31 does not change the condition or make the branch wait for the GTE.
See the [hardware reference](https://psx-spx.consoledev.net/dev/ps1/cpu/cpuspecifications/#coprocessor-branches).

A disabled coprocessor raises a coprocessor-unusable exception before executing
the slot. COP0 branches require CU0 even in kernel mode; COP0 register moves
retain their kernel-mode access rule. Interpreter, cached and native execution
share the branch classification and target calculation. Native branches compile
their slot and preserve its exception context and pending loads.

Generated cases check all four `rt` forms, forward and backward targets,
status-register changes, slot exceptions, GTE busy state and FLAG. They require
native progress without interpreted instructions in the native configurations.
The original published branch program provides an additional instruction-stream
check. These cases cover the documented `rs=8` encoding and do not establish
complete CPU timing or other raw encoding aliases.

## Data-access exceptions

Native loads and stores save register writes, pending loads, instruction context
and busy-unit deadlines before accessing memory. A reported bus failure exits
the block before a load result, GTE register write or following instruction can
change state. Halfword and word alignment checks also apply to mapped accesses
when the optional recompiler memory-exception setting is disabled.

Generated tests cover byte, halfword, word, merge and GTE transfers, constant and
dynamic addresses, register zero, and taken and untaken branch slots. They check
RAM and device alignment errors, injected device failures, pending loads and
exception metadata. Native mmap tests require observed backpatches for dynamic
device accesses. Consecutive-store tests check cache isolation and ordinary RAM
writes without relying on an accidental mmap fault to select isolated handlers.
Successful merge cases check both instruction orders at every word offset,
neighboring store bytes, and a pending load into the address register.

These cases verify error propagation within the current bus model. Merge
operations still use aligned word reads and read-modify-write stores; their
device access widths remain unqualified. GTE transfer timing and physical bus
error decoding also need separate hardware checks.

## Instruction fetch

Failed instruction reads stay pending until the faulting instruction reaches
execution. A jump retires its link and delay slot before a target fetch fault;
a faulting delay slot records the branch PC and target. Instruction bus errors
leave `BadVaddr` unchanged. Alignment errors record the faulting fetch address.
Failed cache fills remain invalid, and a later cache write cannot repair an
already failed prefetch.

RAM and BIOS reads use their backing storage. Expansion fetches currently
return zero-filled words. The tested SPU and DMA register regions use device
word-read handlers during execution, including
cache fills. Speculative block decoding does not read device registers, so
these regions run through shared execution. Existing bus handlers supply their
access costs; exact instruction-fetch timing for I/O still needs hardware
qualification.

Save-state version 85 stores the pending fetch exception with the pipeline.
Version 84 has no such field and loads with no pending fetch exception.

The two cached address regions share physical instruction-cache tags. A jump
between KUSEG and KSEG0 keeps cached words after a RAM write; KSEG1 bypasses
the cache. Different physical RAM mirrors still have distinct tags. The
virtual execution address remains intact for branches and exceptions.

Save-state version 86 stores physical cache tags. Loading an older stream
converts its KSEG0 tags while retaining cached words and per-word validity.
Cache-control bit 11 selects instruction-cache use in cached address regions.
When it is clear, instruction reads bypass the cache without filling or
clearing its words and tags. Re-enabling it exposes retained cache contents.
The instruction already prefetched stays latched across a guest control write.
Generated tests check that ordering across execution modes; the exact hardware
latency of adjacent cache-control writes still needs independent qualification.
Programmable refill sizes and tag-test mode also remain unqualified.

Save-state version 87 retains the native pipeline's fetched word. Earlier
native streams rebuild it from the saved execution PC, except when the stream
already contains a pending branch slot or fetch exception. The CPU serializer
keeps pending loads and GTE/multiply/divide deadlines during restoration.

## Recompiler backends

| Backend | Status | Files |
|---------|--------|-------|
| x86-64 | Native-code backend with instruction codegen, fastmem, icache and PGXP hooks. | `src/core/cpu/backends/x64/x64_emit.c`, `src/core/cpu/backends/x64/x64_backend.c` |
| AArch64 | Lifecycle and dispatcher with a per-block interpreter fallback. Per-op codegen, fastmem, icache and PGXP hooks are NULL. | `src/core/cpu/backends/arm64/arm64_emit.c`, `src/core/cpu/backends/arm64/arm64_backend.c` |

## PGXP

PGXP (Parallel/Precision Geometry Transformation Pipeline) tracks subpixel-precision floating-point shadows of CPU GPRs, COP0, and GTE registers across loads/stores for optional subpixel geometry handling. Hooks fire from all three CPU modes; the recompiler emits inline calls into the PGXP runtime. Toggle via `--pgxp on|off` or settings INI. Implementation: `src/core/cpu/cpu_pgxp.c`.

## Recompiler fastmem

`--fastmem` selects how the JIT dispatches loads and stores:

| Mode | Strategy |
|------|----------|
| `off` | Always go through the C helper (slow path). |
| `lut` | Requested LUT mode (default). The x86-64 backend currently uses the slow C helper path; native LUT execution is unfinished. |
| `mmap` | Direct host addressing with `SIGSEGV` handling for out-of-region accesses through `src/util/platform/page_fault_handler.c`. |

## Execution comparisons

`make test-diff` compares bounded ordinary/cached instruction traces.
`make recomp-diff-test` compares independently saved machine checkpoints,
including serialized timing. Missing progress and any differing data fail.
The tools do not replay instructions on live devices. See
[comparison coverage and limits](../development/comparing-execution.md).
