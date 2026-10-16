# Building cupid-ps1

The supported build is Linux x86-64 with a C11 compiler, GNU Make and SDL2.
On Windows, use a Linux environment such as WSL. Native Windows support is
to be implemented in [issue #13](https://github.com/cupidthecat/cupid-ps1/issues/13),
including a native build, platform backends, Windows CI and runtime checks.
The bundled audio library is currently a Linux x86-64 binary.

On Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install build-essential clang pkg-config libsdl2-dev \
  libzstd-dev liblzma-dev zlib1g-dev libgl-dev libegl-dev libx11-dev libudev-dev
make -j2 all hcheck
./build/cupid-ps1 --bios /path/to/your/bios.bin /path/to/your/game.cue
```

The build stages the bundled audio library into `build/lib/` using its ELF
SONAME. Keep that directory beside the executable. Each build directory has
its own staged library; no manually created linker aliases are needed.
Existing dependency licenses remain in `licenses/` and `dep/`.

Use separate directories when changing compilers or instrumentation:

```sh
make -j2 CC=gcc BUILD=build/gcc all hcheck test
make -j2 CC=clang BUILD=build/clang all hcheck test
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ASAN_OPTIONS=halt_on_error=1 \
make -j2 CC=clang BUILD=build/sanitize OPT='-O1 -g' \
  SANITIZERS=address,undefined all hcheck test
```

Header dependencies are tracked automatically. Changing compiler flags in an
existing directory requires a clean build. Remove generated output with
`make clean`, or `make BUILD=build/clang clean` for a named directory.

The rasterizer includes x86 SIMD translation units. The vector test binary
requires an AVX2-capable host. Other host architectures have not been
qualified by this build workflow.

For an OpenGL context smoke test:

```sh
make gl-smoke
SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 ./build/cupid-ps1-gl-smoke
```

This runtime check needs a working X11 display. Building the smoke binary
does not test context creation or rendering.

Keep BIOS files, game images, memory cards and save states local. They are
excluded from new commits. Existing published history may still contain old
assets; the build changes do not rewrite that history.

See [testing](testing.md) for the validation scope and
[formatting](formatting.md) for the source formatter.
