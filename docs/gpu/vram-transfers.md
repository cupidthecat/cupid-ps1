# VRAM uploads

CPU-to-VRAM commands use ten width bits and nine height bits. A zero in either
masked field selects its maximum size. Upper parameter bits do not select a
different operation: `FFFFFFFF` and `01FF03FF` both describe 1023 by 511 pixels.
See the [copy parameter specification](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#masking-for-copy-commands-parameters).

Uploads consume complete 32-bit words. For an odd pixel count, the final high
halfword is padding. Pixel words remain data even when their high byte resembles
a GPU command. Coordinates wrap independently at the horizontal and vertical
VRAM boundaries. Drawing-area clipping and offsets do not affect the copy.

The component regressions check every VRAM pixel after full uploads through
register and DMA submission, equivalent size encodings, odd padding and
continuation after resetting away a saved pending upload. Generated guest
firmware checks command-shaped data and exact saved-state continuation across
CPU modes, native memory paths and block-linking settings on Linux and Windows.

These checks cover the implemented transfer model. Physical transfer timing,
partial-write visibility and the status signals during uploads remain
unqualified. No new physical-console capture was made for the all-ones size.
