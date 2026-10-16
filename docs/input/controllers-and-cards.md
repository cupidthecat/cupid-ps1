# Controllers and memory cards

## Controllers

Controller types are defined in `src/core/shared/types.h`.

| Type | Implementation notes | Source |
|------|--------|----------------|
| `NONE` | n/a | n/a |
| `DIGITAL_CONTROLLER` | Implemented | `src/core/input/controllers/digital_controller.c` |
| `ANALOG_CONTROLLER` (DualShock) | Implemented. Default for slot 1. Vibration supported. | `src/core/input/controllers/analog_controller.c` |
| `ANALOG_JOYSTICK` (SCPH-1110 Flightstick) | Implemented. Dual-stick + mode toggle. | `src/core/input/controllers/analog_joystick.c` |
| `GUNCON` | Implemented. Trigger / A / B / ShootOffscreen. CRTC beam hit-test via `gpu_query_light_gun_position()`. SDL mouse drives the pointer. | `src/core/input/controllers/guncon.c` |
| `PLAYSTATION_MOUSE` | Implemented. Two buttons + relative pointer with sensitivity. SDL mouse-motion deltas feed into the SIO transfer. | `src/core/input/controllers/playstation_mouse.c` |
| `NEGCON` / `NEGCON_RUMBLE` | Implemented | `src/core/input/controllers/negcon.c` |
| `JUSTIFIER` | Implemented. Konami lightgun with IRQ10 pulse on the matching CRTC line, scheduled through `timing_event_schedule()`. | `src/core/input/controllers/justifier.c` |
| `POPN_CONTROLLER` | Implemented: 9-button mask over the digital pad. | `src/core/input/controllers/digital_controller.c` |
| `DDGO_CONTROLLER` (Densha de Go) | Implemented | `src/core/input/controllers/ddgo_controller.c` |
| `JOGCON` | Implemented: jog wheel + MODE. | `src/core/input/controllers/jogcon.c` |

Multitap is wired in `src/core/input/multitap.c`, supporting up to 8 controller+memcard ports total.

## Serial mode registers

The controller port (SIO0) and serial port (SIO1) retain different MODE bits:

| Register | Address | Writable mask |
|----------|---------|---------------|
| `JOY_MODE` | `0x1F801048` | `0x013F` |
| `SIO_MODE` | `0x1F801058` | `0x00FF` |

Both ports retain the baud factor, character length and parity fields. SIO0
also retains clock polarity at bit 8; SIO1 retains stop-bit length at bits
6-7. Other bits read as zero. See the [serial register specification](https://psx-spx.consoledev.net/serialinterfacessio/#0x1f801048-0x10n-mr-mode-register).

Reset clears MODE. Save states keep the same register layout; loading an older
state clears reserved MODE bits while preserving the supported fields.
Set `CUPID_TRACE=1` to log MODE reads and writes when diagnosing bus accesses.
These register checks do not establish complete controller or serial-port
emulation.

## Memory cards

- 128 KB image: 16 frames × 64 sectors × 128 bytes = 131 072 bytes.
- File extension `.mcd`. Default path for slot 1: `./memcards/card1.mcd`.
- Lazy write-back: dirtied cards are flushed asynchronously via a timing event (`src/core/input/memory_cards/memory_card.c`).
- Types: `NONE`, `SHARED`, `PER_GAME`, `PER_GAME_TITLE`, `PER_GAME_FILE_TITLE`, `NON_PERSISTENT`. Default for slot 1: `PER_GAME_TITLE`.
- Up to 8 slots total via multitap on each port.
