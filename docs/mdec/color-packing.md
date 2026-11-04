# MDEC color packing

The legacy color converter stores each RGB component in one byte before
packing output pixels. It truncates signed values to eight bits so their
sign bits cannot overwrite neighboring components. The 24-bit output
contains three consecutive color bytes. The 15-bit output extracts five
bits per component and applies the selected bit-15 value.

For example, a generated stream produced legacy components (-11, 5, 0).
Their signed bytes are F5/05/00. The earlier 16-bit casts produced
F5/FF/00, replacing green with the high bits of red. This fix changes
component packing while preserving the converter's existing arithmetic.

The `MdecColorPacking` C-test filter decodes six complete blocks for each
generated color vector. Five vectors cover negative channels, mixed signs
and positive controls. Cases vary signed output, 15- or 24-bit packing,
the bit-15 setting, manual or DMA input and reads, and component-state
restoration. They check every packed pixel and output byte. Restoration
preserves every serialized decoder byte and the pending copy-out deadline.

These tests use the scheduler's actual copy-out event. They leave
conversion precision, IDCT rounding and physical transfer timing
unqualified. The [MDEC register and format documentation](https://psx-spx.consoledev.net/macroblockdecodermdec/)
describes the output controls and the remaining precision uncertainty.
No new console capture is available.
