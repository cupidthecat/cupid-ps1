# CPU execution

Defined in `src/core/types.h`, dispatched by `cpu_execute()` in `src/core/cpu_core.c`.

| Mode | What it is |
|------|-----------|
| `interpreter` | MIPS R3000A interpreter; also used by the trace diagnostics. |
| `cached_interpreter` | Block-level decode cache wrapping the interpreter. Execution-path consistency still needs qualification. |
| `recompiler` | JIT to native code. Default on x86-64 hosts. The AArch64 backend is scaffold-only (per-block interpreter fallback) and not actively maintained; ARM hosts are not qualified by the current build workflow. |

## Recompiler backends

| Backend | Status | Files |
|---------|--------|-------|
| x86-64 | Native-code backend with instruction codegen, fastmem, icache and PGXP hooks. | `src/core/x64_emit.c`, `src/core/x64_backend.c` |
| AArch64 | Scaffold only. Lifecycle + dispatcher + per-block interpreter fallback. Per-op codegen / fastmem / icache / PGXP hooks all NULL. **Not actively worked on.** | `src/core/arm64_emit.c`, `src/core/arm64_backend.c` |

## PGXP

PGXP (Parallel/Precision Geometry Transformation Pipeline) tracks subpixel-precision floating-point shadows of CPU GPRs, COP0, and GTE registers across loads/stores for optional subpixel geometry handling. Hooks fire from all three CPU modes; the recompiler emits inline calls into the PGXP runtime. Toggle via `--pgxp on|off` or settings INI. Implementation: `src/core/cpu_pgxp.c`.

## Recompiler fastmem

`--fastmem` selects how the JIT dispatches loads and stores:

| Mode | Strategy |
|------|----------|
| `off` | Always go through the C helper (slow path). |
| `lut` | LUT lookup keyed on bits 24..28 of the address. Default. |
| `mmap` | Direct host addressing with `SIGSEGV` handling for out-of-region accesses. Lowest overhead; uses the page-fault handler in `src/util/page_fault_handler.c`. |

## Diff modes

- `make test-diff` runs the interpreter twice with `CUPID_DIFF_TRACE` set and byte-compares the per-instruction trace.
- `make recomp-diff-test` runs the recompiler with `CUPID_RECOMP_DIFF=1`. Each compiled block snapshots GPRs at prologue and re-executes the same instructions on a scratch interpreter copy, aborting on register divergence.

The trace and block-comparison tools still need progress and state-coverage qualification. See [testing limits](../development/testing.md).
