# Native Windows platform components

The Windows x86-64 platform layer supports an experimental native emulator
build and standalone C11 checks. See the [build instructions](building.md).
Remaining runtime qualification is tracked in
[issue #13](https://github.com/cupidthecat/cupid-ps1/issues/13).

## Build and check

Use CMake, Ninja and the MSYS2 MinGW64 compiler. In PowerShell:

```powershell
$env:PATH = 'C:\msys64\mingw64\bin;' + $env:PATH
cmake -S tests/platform/windows -B build/windows-platform -G Ninja `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe
cmake --build build/windows-platform --parallel 2
ctest --test-dir build/windows-platform --output-on-failure
```

Hosted CI builds the same components with project warnings treated as
errors, compiles the shared headers separately and runs all six test
executables. These checks use generated data and a generated DLL. They do
not require a BIOS or disc.

## Implemented interfaces

- Shared views use pagefile-backed sections. The arena uses Windows
  placeholders to keep unmapped ranges reserved while mapped views are
  removed or replaced. Range records track ownership, splitting and merging.
  This needs the [placeholder APIs available since Windows 10 version 1803](https://devblogs.microsoft.com/oldnewthing/20231229-00/?p=109204).
- Executable storage is allocated near the process image when available.
  Instruction-cache writes are flushed through the Windows API.
- A vectored exception handler supplies the faulting PC, address and write
  flag to the core callback. Unhandled faults continue through the Windows
  handler chain. A thread-local guard allows a later handler to process a
  nested fault. Uninstall removes only the handler owned by this layer.
- Monotonic timestamps retain the common nanosecond unit. Windows reads its
  performance counter and sleeps using waitable timers. Past exact deadlines
  below the spin guard return without unsigned wraparound.
- Native threads use CRT thread entry, owned handles for join/detach and
  kernel semaphores. CPU-time queries use 100 ns units; callers obtain the
  unit rate through the existing interface. Borrowed thread identities remain
  valid while their threads are alive. Affinity uses the caller's 64-bit mask;
  cross-group affinity on systems with more than 64 processors is unqualified.
- Library names and module paths cross the Windows API boundary as UTF-16
  converted from UTF-8. Failed loads preserve an already loaded module.
- File streams and directory operations convert UTF-8 paths to wide Windows
  APIs, including long drive and UNC paths. Metadata and offsets retain 64-bit
  sizes. File sharing, exclusive creation and replacement preserve open-file
  ownership. Recursive deletion removes directory junctions without traversing
  their targets; recursive searches track directory identities to avoid loops.
- Save-state wrappers use seekable anonymous streams and read-only buffer
  streams. Aligned buffers use matching Windows allocation/free functions.
  Crash diagnostics report exceptions and cleanup to stderr.
- The native entry point converts Unicode command-line arguments to UTF-8.
  Generated x64 calls use Windows argument registers, nonvolatile register
  classification and caller home space. Execution exits bypass SEH unwinding
  across generated frames. The bus maps its RAM aliases into owned placeholders.

The implementations live in `src/common/windows/` and
`src/util/platform/windows/`. The Linux build retains its existing platform
implementations. The shared permission bits retain their Linux values, with
explicit Windows protection conversion.

## Validation scope

The memory executable checks coherent aliases in a 4 GB reservation,
page-aligned view replacement, failed mapping recovery, invalid ranges,
Unicode named sections and exclusive creation. It checks generated x86-64
loads and stores across protection faults, exact fault PCs, read/write flags,
independent aliases, nested faults and preservation of later handlers. A child
process verifies that an unhandled fault exits with an access violation.

The runtime executable checks monotonic reads, sleep deadlines, CPU-time
accounting, thread identity, join/detach, semaphore counts and concurrent
protected-page writes. The loader executable calls an exported C function
from a generated DLL with a Unicode filename and checks failed loads,
ownership transfer and symbol lookup.

The filesystem executable checks binary bytes, Unicode and long names,
sparse offsets beyond 4 GB, mandatory sharing, replacement over an open target,
temporary cleanup and junction traversal/deletion boundaries.
Filename buffers retain their terminating wide character when passed through
the [native rename interface](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info).
The filesystem check fills allocation padding with nonzero bytes to expose
native path conversion reading beyond the supplied buffer. The path
executable checks drive/UNC roots, parent components, mixed separators and
aligned allocation. An isolated crash child checks exception reporting,
one cleanup invocation, repeated installation and previous-filter chaining.

Generated firmware tests exercise the integrated CPU, bus and state paths;
see [their commands and scope](building.md#automated-checks). Dummy SDL drivers
do not qualify native desktop video/audio, physical input, persistent cards or
gameplay. The experimental OpenGL backend has no Windows context implementation;
Windows uses software rendering. Physical CD access returns an explicit error.
Disc-image readers remain shared. See the
[audio dependency recipe](../dependencies/soundtouch.md) for its separate checks.
