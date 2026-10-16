# cupid-ps1

A PlayStation 1 emulator written in C11 with an SDL2 frontend.
The supported build is Linux x86-64; Windows users can build through WSL.

The emulator is under development. It includes an interpreter, cached
interpreter, x86-64 recompiler, software renderer and experimental OpenGL
renderer. Complete hardware accuracy and game compatibility remain unverified.

## Build and run

Install the [build dependencies](docs/development/building.md), then run:

```sh
make -j2 all hcheck
./build/cupid-ps1 --cpu-mode interp --gpu-renderer software --pgxp off \
  --bios /path/to/your/bios.bin /path/to/your/game.cue
make test
```

Supply your own BIOS and game files. Keep them outside the tracked source tree.
Use native timing, software rendering and PGXP disabled when checking core
behavior. See [testing and validation limits](docs/development/testing.md).

## Controls

| Key | Action |
|-----|--------|
| Arrow keys | D-pad |
| `Z` / `X` / `A` / `S` | Cross / Circle / Square / Triangle |
| `Enter` / `Backspace` | Start / Select |
| `LShift` / `LCtrl` / `RShift` / `RCtrl` | L1 / L2 / R1 / R2 |
| `Esc` | Request quit |
| `P` / `F5` | Pause or resume / reset |
| `F11` / `F12` | Save / load the selected state slot |
| `Shift+1` ... `Shift+9` | Select state slot |

See [frontend usage](docs/frontend/usage.md) for all options, hotkeys,
controller mappings and settings.

## Documentation

- [Building](docs/development/building.md), [testing](docs/development/testing.md)
  and [formatting](docs/development/formatting.md)
- [CPU execution](docs/cpu/execution.md) and [rendering](docs/gpu/rendering.md)
- [Audio output](docs/audio/output.md) and [BIOS boot](docs/bios/boot.md)
- [Disc images and changing discs](docs/media/disc-images.md)
- [Controllers and memory cards](docs/input/controllers-and-cards.md)
- [Save states](docs/state/save-states.md)
- [Game database](docs/development/game-database.md),
  [debugging](docs/development/debugging.md) and
  [source layout](docs/development/source-layout.md)

Licensing and required attribution are in [LICENSE](LICENSE) and
[licenses/](licenses/).
