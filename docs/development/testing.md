# Testing cupid-ps1

Run the automated suite from the repository root:

```sh
make test
bash scripts/run_tests.sh YamlParser
bash scripts/run_tests.sh GameDatabase CC=clang BUILD=build/clang
```

Each C test runs in a child process. A failed assertion or signal fails the
suite. A filter that selects no tests also fails. The ISO9660 database
lookup tests generate their own metadata-only discs in temporary
directories. The suite requires no commercial disc or BIOS file.

The suite covers utility code, image parsing, database loading and caching,
software rasterizer operations, scalar/SIMD comparisons, and x86 emitter
checks. It does not establish complete CPU, GPU, audio, timing or gameplay
accuracy. Database compatibility ratings describe the bundled data and
are not test results for this emulator.

Hosted CI builds the emulator, standalone headers and OpenGL smoke binary,
then runs the suite using GCC, Clang, and Clang with address and undefined
behavior sanitizers. It checks test selection failures and the filtered
runner. It does not boot private inputs, run an OpenGL context, or upload
runtime files.

The `CacheBiu` C-test filter checks isolated cache metadata, code access widths,
partial refills and state restoration. Its guest program changes BIU modes
under ordinary, cached and native execution, including mmap and unlinked native
configurations. These generated cases complement the locally run published
cache program. See [CPU execution](../cpu/execution.md#instruction-cache-tag-accesses-and-refills)
for the behavior covered and the remaining limits.

The `ReservedInstruction` filter checks reserved encodings, stale cached words,
branch exception context and saved prefetch restoration. The Python suite also
boots generated firmware that rewrites RAM beneath a cached reserved word and
checks the saved exception results in interpreter, cached and native configurations.
The generated inputs require no commercial BIOS or disc.

The `FastmemMapping` filter checks RAM aliases, code-page protection and remapping
through Linux mapping permissions. It also checks configured 8 MB offsets and
unmapped holes. See [memory mapping](../memory/mapping.md) for the access paths
and qualification limits.

The `DeviceBus` filter checks transfer strobes, BIOS fetches, SPU registers and
CD-ROM response and parameter FIFOs. Its guest programs change device controls
under every CPU execution mode and continue after component-state restoration.
See [device bus accesses](../memory/device-bus.md) for the implemented behavior
and the configurations and timing that remain unqualified.

The `CpuPartial` filter checks shifted stores to internal word latches and
16-bit SPU registers. It checks neighboring registers, address stepping,
component-state continuation and the absence of extra device reads. See
[partial stores](../memory/partial-stores.md) for the remaining access paths.

The `NativeLut` filter executes byte, halfword and word accesses through all RAM
aliases, unaligned merges and coprocessor transfers. It checks direct-access
counts, recovery through bus handlers, code writes, isolation changes and
configured 8 MB backing. The `_lut` filter also selects LUT variants of the
exception, permission, branch, cache-control and retained-fetch regressions.
Generated firmware compares linked and unlinked LUT runs across save/restore.
These checks cover the generated programs; they do not establish complete CPU
accuracy or qualify physical 8 MB hardware.

For a bounded save/load smoke check with authorized local inputs, run:

```sh
python3 scripts/runtime_smoke.py --bios /path/to/bios.bin \
  --mode interp --revision COMMIT_SHA \
  --hash-input /path/to/game.bin /path/to/game.cue
```

Repeat with `--mode cached` and `--mode recomp` to check the other execution
paths. The runner uses native timing, scalar software rendering, PGXP off,
fastmem off, dummy SDL video/audio drivers and a fresh settings directory.
It removes inherited `CUPID_*` overrides. Supply each referenced media file
with `--hash-input` so its hash is recorded too.

Each run creates a new directory under `.local-research/` containing the
log, input and binary hashes, result JSON, state and memory cards. An existing
output directory is rejected. The default deadline is 60 seconds; `--timeout`
sets another deadline. A timeout fails even if the process exits successfully
after termination. Success requires one save/load observation at frame 300 or
later, at least 60 continuation frames, one completion message after teardown,
a nonempty state file and process exit 0.
Cached mode also requires positive block creation, reuse and execution counts,
so ordinary interpreter fallback cannot qualify as cached execution.

These checks exercise save/load and shutdown. Dummy drivers do not verify
displayed video or audible output, and the check does not compare restored
machine state against an independent execution. The runner's failure handling
is tested in CI with generated inputs and substitute processes:

```sh
python3 -m unittest discover -s tests -p 'test_runtime_smoke.py'
```

Use [execution comparisons](comparing-execution.md) to compare bounded,
independent runs. Instruction comparisons require exact trace counts and
completion; checkpoint comparisons retain every saved byte and report any
difference. Both require normal exit and positive execution progress. CI
checks false-success cases and boots generated firmware with all three CPU
modes, including native linking and fastmem configurations.

For runtime qualification, retain the tested commit, input hashes,
execution and rendering modes, settings, observations and failures in an
ignored working directory. Use native timing and software rendering as the
baseline. A successful boot or save/load smoke run proves only those
observations; it does not prove completion of a game or state equivalence.
