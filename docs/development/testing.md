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

The existing `test-diff`, `recomp-diff-test` and `recomp-diff-ticks-test`
targets are diagnostic tools requiring local authorized inputs. Their
progress detection and comparison coverage still need qualification.
In particular, elapsed runtime and the absence of a reported divergence
do not prove that any instructions or blocks were compared. Do not use
these targets as compatibility or accuracy acceptance evidence yet.

For runtime qualification, retain the tested commit, input hashes,
execution and rendering modes, settings, observations and failures in an
ignored working directory. Use native timing and software rendering as the
baseline. A successful boot or save/load smoke run proves only those
observations; it does not prove completion of a game or state equivalence.
