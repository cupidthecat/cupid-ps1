# Comparing execution

Run `scripts/compare_execution.py` in the Linux build environment. It starts
two independent emulator processes with identical input paths, fresh settings
and separate memory cards. Each invocation requires a new output directory.
The default output is a unique directory under `.local-research/`.

For an ordinary-versus-cached instruction comparison:

```sh
python3 scripts/compare_execution.py --bios /path/to/bios.bin \
  --hash-input /path/to/game.bin --instructions 50000 \
  --revision COMMIT_SHA /path/to/game.cue
```

`--left interp --right interp` checks repeatability within the interpreter.
Instruction traces support `interp` and `cached`. Each trace must contain the
exact requested number of consecutive records, the expected schema and a
completion footer. Both processes must exit normally before their deadlines.
The trace stores PC, instruction, all 32 GPRs, HI/LO, 11 COP0 words, 64 GTE
words, both pending loads, CPU pending ticks and scheduler global ticks.
It records these values after prefetch and before executing each instruction.
The final instruction executes before the process requests shutdown.

This trace excludes other CPU pipeline fields, caches, scratchpad, RAM,
devices and the scheduler event queue. It cannot establish complete machine
state equality. Native instruction tracing is rejected because fallback-only
records would omit compiled instructions.

For checkpoints across any two execution modes:

```sh
python3 scripts/compare_execution.py --kind state --right recomp \
  --bios /path/to/bios.bin --hash-input /path/to/game.bin \
  --interval 60 --checkpoints 5 --revision COMMIT_SHA /path/to/game.cue
```

This saves independent files at frames 60, 120, 180, 240 and 300. By default,
both runs continue without loading a state. The tool compares every
byte of each uncompressed save-state file, plus the frame and global tick
observations. It checks headers, sizes, checkpoint order and completion.
A differing byte fails the comparison and records its first file offset.
It does not omit timing fields, pipeline/cache contents or the execution-mode
flag to make different runs match.

To check save-state continuation within one execution mode:

```sh
python3 scripts/compare_execution.py --kind state --left recomp --right recomp \
  --restore-at 2 --interval 60 --checkpoints 5 --bios /path/to/bios.bin \
  --hash-input /path/to/game.bin --revision COMMIT_SHA /path/to/game.cue
```

The right process reloads its second checkpoint at frame 120. The left process
continues uninterrupted. The runner requires one successful reload at the
expected frame and tick, followed by all remaining checkpoints. It compares
every saved byte, including the later continuation files. `--restore-at` must
precede the final checkpoint and both modes must match. A successful load or
continued frame count alone does not establish equivalent continuation.

The file comparison covers the version 84 save-state serializers, including
CPU, GTE, RAM, VRAM, device state, controller/card data and event records.
It excludes host mappings and dispatch flags, generated code, CPU PGXP shadow
arrays not written by the CPU serializer, external BIOS/disc contents, host
audio/video/input queues and asynchronous reader internals. Equality applies
to serialized data only. A mismatch can include engine bookkeeping or CPU
progress beyond an event boundary; inspect it before attributing it to a
hardware defect. Matching frame/global ticks alone is insufficient.

`--fastmem off|lut|mmap` and `--no-linking` select the native configuration.
Every result checks the reported execution configuration. The current x86-64
backend reports slow-path execution for a LUT request, so a LUT qualification
fails until [native LUT accesses](https://github.com/cupidthecat/cupid-ps1/issues/15)
are implemented. Cached mode requires
positive block creation, reuse and execution counts. Native mode requires
positive native entry and completion counts. `--minimum-blocks` sets the
required block count (default 100). These are path/progress observations,
not hardware-accuracy measurements.

Each `result.json` records the comparison scope and exclusions, source revision
when supplied, command, configuration, binary/input hashes, artifact hashes,
process exit codes, deadlines, counts and differences. Supply every referenced
media file using repeated `--hash-input` options. Inputs and binary are hashed
again after both runs. Missing inputs, reused output directories, startup
failures, incomplete traces/checkpoints, insufficient progress, timeouts and
unequal data all fail. A timeout fails even if termination returns exit 0.

The Make targets use the same runner:

```sh
make test-diff DIFF_BIOS=/path/to/bios.bin DIFF_GAME=/path/to/game.cue \
  DIFF_HASH_INPUT=/path/to/game.bin
make recomp-diff-test DIFF_BIOS=/path/to/bios.bin DIFF_GAME=/path/to/game.cue
```

`recomp-diff-ticks-test` is an alias for the full checkpoint comparison, which
already compares serialized timing. `DIFF_SECONDS`, `DIFF_LINES`,
`DIFF_INTERVAL`, `DIFF_CHECKPOINTS`, `DIFF_MIN_BLOCKS`, `DIFF_FASTMEM`,
`DIFF_LINKING=off` and `DIFF_OUTPUT` override the corresponding options.
`DIFF_LEFT` and `DIFF_RIGHT` choose instruction-trace modes.

The old `CUPID_RECOMP_DIFF` live replay and `RecompilerCompareRegisterFiles`
setting are rejected. The old replay restored CPU and RAM while leaving device
side effects live, and its linked/fastmem snapshots were not reliably paired.
The independent runner removes that restoration/pairing requirement.

CI exercises generated firmware loops and loader inputs, ordinary/cached
comparisons, uninterrupted/restored continuation in all three modes, native
repeatability with linking and mmap fastmem, rejection of
an unimplemented LUT request, and native-versus-interpreter checkpoint
mismatches. These checks cover those
programs and configurations. They do not establish native/interpreter equality
for general code, full hardware timing, gameplay compatibility or audible and
displayed output. Private BIOS, discs, checkpoints and logs stay local.
