# MDEC coefficient arithmetic

The scalar RLE decoder expands signed 10-bit input coefficients before the
IDCT. Unscaled coefficients are multiplied by 32. Quantized coefficients
use the table and scale, then multiply by 16 before applying the existing
sign-dependent bias and clamp.

Left-shifting a negative signed integer is undefined in C11. The decoder
uses multiplication for these steps. The input, table and scale ranges
keep every intermediate within a signed 32-bit integer. Rounding, bias,
clamp limits and coefficient positions stay the same.

The `MdecSigned` C-test filter covers negative extremes, positive values,
zero and saturation. Generated command streams load quantization tables
and pause in an incomplete block so the tests can inspect all 64 stored
coefficients. Manual and DMA input cases also check the remaining count
and coefficient position. The fixture restores CPU, decoder, DMA and
scheduler state, then compares every serialized decoder byte.

These cases preserve the current scalar results and check that sanitizer
runs accept the arithmetic. Complete MDEC decoding and physical
quantization, IDCT and color-conversion accuracy remain unqualified.
Scale/table-zero interactions and output timing still need separate
qualification. No new console capture is available.
