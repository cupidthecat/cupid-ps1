# CPU execution

Defined in `src/core/types.h`, dispatched by `cpu_execute()` in `src/core/cpu_core.c`.

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

Ordinary and cached interpretation share a prefetch pipeline. Switching between
these modes retains prefetched words and instruction-cache data. Before native
dispatch starts, pending branch-delay instructions finish through the shared
executor. Leaving native execution rebuilds prefetch at the current PC and
preserves pending loads. Native blocks maintain cache tags without line data,
so transitions involving native execution invalidate that cache. This remains
a limitation of the native cache model.

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

## Recompiler backends

| Backend | Status | Files |
|---------|--------|-------|
| x86-64 | Native-code backend with instruction codegen, fastmem, icache and PGXP hooks. | `src/core/x64_emit.c`, `src/core/x64_backend.c` |
| AArch64 | Lifecycle and dispatcher with a per-block interpreter fallback. Per-op codegen, fastmem, icache and PGXP hooks are NULL. | `src/core/arm64_emit.c`, `src/core/arm64_backend.c` |

## PGXP

PGXP (Parallel/Precision Geometry Transformation Pipeline) tracks subpixel-precision floating-point shadows of CPU GPRs, COP0, and GTE registers across loads/stores for optional subpixel geometry handling. Hooks fire from all three CPU modes; the recompiler emits inline calls into the PGXP runtime. Toggle via `--pgxp on|off` or settings INI. Implementation: `src/core/cpu_pgxp.c`.

## Recompiler fastmem

`--fastmem` selects how the JIT dispatches loads and stores:

| Mode | Strategy |
|------|----------|
| `off` | Always go through the C helper (slow path). |
| `lut` | Requested LUT mode (default). The x86-64 backend currently uses the slow C helper path; native LUT execution is unfinished. |
| `mmap` | Direct host addressing with `SIGSEGV` handling for out-of-region accesses through `src/util/page_fault_handler.c`. |

## Execution comparisons

`make test-diff` compares bounded ordinary/cached instruction traces.
`make recomp-diff-test` compares independently saved machine checkpoints,
including serialized timing. Missing progress and any differing data fail.
The tools do not replay instructions on live devices. See
[comparison coverage and limits](../development/comparing-execution.md).
