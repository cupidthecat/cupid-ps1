# Building the audio dependency

The audio frontend uses SoundTouch's floating-point C interface. A source
build is available for version 2.3.3, pinned to commit
`e83424d5928ab8513d2d082779c275765dee31b9` in its
[official repository](https://codeberg.org/soundtouch/soundtouch).
The build script checks the archive's SHA-256 before extraction. It changes
the resource include from `afxres.h` to the Windows SDK's `winres.h`, so the
DLL does not require MFC. That source change is recorded in the build receipt.

Use Python 3.12 or newer, CMake, Ninja and a C++ compiler:

```sh
python3 scripts/dependencies/build_soundtouch.py
cmake -S tests/dependencies -B build/audio-probe -G Ninja \
  -DSOUNDTOUCH_BUILD="$PWD/build/dependencies/soundtouch-2.3.3/build"
cmake --build build/audio-probe
ctest --test-dir build/audio-probe --output-on-failure
```

On native Windows with the MSYS2 MinGW64 compiler, from PowerShell:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
python scripts/dependencies/build_soundtouch.py --compiler C:/msys64/mingw64/bin/g++.exe
$dependency = (Resolve-Path build/dependencies/soundtouch-2.3.3/build).Path
cmake -S tests/dependencies -B build/audio-probe -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe "-DSOUNDTOUCH_BUILD=$dependency"
cmake --build build/audio-probe
ctest --test-dir build/audio-probe --output-on-failure
```

With Visual Studio 2022 C++ tools, pass
`--generator "Visual Studio 17 2022"` to the script and use that generator
for the probe. Build with `--config Release` and run CTest with `-C Release`.
Hosted Windows CI checks this configuration.

The default output is `build/dependencies/soundtouch-2.3.3/`. The script
requires a new output directory and writes a source hash and command receipt.
Use `--output` for a separate build and `--archive` for a cached archive;
cached inputs must pass the same hash check. Generated files and dependency
sources stay outside commits. MinGW DLLs also need the compiler's runtime
libraries available on `PATH`.

The probe compiles the project's wrapper header as C11 and checks instance
creation, stereo sample submission, flush, finite nonzero output, frame
count, clear and destruction. This checks the dependency interface; it does
not qualify SPU accuracy, audible output or a native Windows emulator.

The existing Linux build still stages its bundled ELF runtime. The source
recipe creates a separate C wrapper library and does not replace that file
or establish a matching build for it. The source archive retains its
copyright and LGPL notices. Keep the source, build recipe and required
notices with any distribution; see [licensing](../../../LICENSE) and
[the dependency notice](../../../licenses/soundtouch-LGPL-2.1.txt).
