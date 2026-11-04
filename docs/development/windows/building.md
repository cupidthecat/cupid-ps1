# Native Windows build

The experimental Windows x86-64 build uses C11, SDL2 and the software renderer.
It runs without WSL. Use the MSYS2 **MinGW64** terminal and a current GCC toolchain.
MSVC, Windows ARM64, native OpenGL contexts and physical CD drives are not
implemented by this build.

## Dependencies and compilation

Install the native packages in MinGW64:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-python mingw-w64-x86_64-SDL2 \
  mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd mingw-w64-x86_64-xz
```

From the repository root, build the pinned audio dependency in a new directory,
then configure the emulator:

```sh
python scripts/dependencies/build_soundtouch.py \
  --output build/dependencies/soundtouch \
  --compiler "$(cygpath -m /mingw64/bin/g++.exe)"
cmake -S . -B build/windows-native -G Ninja -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCUPID_SOUNDTOUCH_ROOT="$(cygpath -m "$PWD/build/dependencies/soundtouch/build")"
cmake --build build/windows-native --parallel 2
./build/windows-native/cupid-ps1.exe --cpu-mode interp --gpu-renderer software \
  --pgxp off --bios 'C:/local/bios.bin' 'C:/local/game.cue'
```

The audio recipe refuses to overwrite an existing output directory. Reuse its
completed build on subsequent emulator builds. See the [recipe](../dependencies/soundtouch.md)
for the pinned source, hash and license. The bundled Linux audio binary is not
used by the Windows build.

Run from MinGW64 so the installed SDL2, compression and compiler runtime DLLs
are on `PATH`. The build copies its audio DLL beside the executable. This is a
development build, not a standalone distributable package; copying only the EXE
to another computer is insufficient. Windows 10 version 1803 or later is needed
for the mapped fast-memory arena.

## Automated checks

```sh
cmake -S tests/platform/windows -B build/windows-platform -G Ninja \
  -DCMAKE_C_COMPILER=gcc
cmake --build build/windows-platform --parallel 2
ctest --test-dir build/windows-platform --output-on-failure
export CUPID_TEST_BINARY="$(cygpath -m "$PWD/build/windows-native/cupid-ps1.exe")"
python -m unittest discover -s tests -p test_frontend_cli.py
python -m unittest discover -s tests -p test_compare_execution_live.py
```

The emulator build compiles project sources and isolated headers with warnings
treated as errors. The generated-input tests require actual CPU progress and
compare complete serialized checkpoints. They exercise interpreter/cached
traces, native calls, LUT/mapped access, device-fault recovery, self-modifying
code, GTE/divide loops, block linking and continuation after restoration.
They use dummy SDL drivers and do not establish desktop presentation, audible
playback, physical controller behavior or commercial-game compatibility.

Local BIOS and disc inputs can also run through `scripts/runtime_smoke.py` and
`scripts/compare_execution.py` on Windows. Keep their output private. Both runners
enforce deadlines and reject unsuccessful exits; Windows copies the public
resource directory into each isolated run without requiring symlink privileges.

See [platform implementation and check scope](platform.md) and
[issue #13](https://github.com/cupidthecat/cupid-ps1/issues/13) for remaining native
runtime qualification. Linux continues to use the existing Makefile.
