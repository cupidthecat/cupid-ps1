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
