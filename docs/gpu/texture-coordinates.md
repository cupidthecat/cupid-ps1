# Rendered texture coordinates

Rectangle texture coordinates repeat after 256 texels on each axis. Reversing
X begins at the odd texel of the source pair, then decrements the byte
coordinate. Reversing Y decrements its original byte coordinate. Texture
windows apply after that byte wrapping, before selecting packed VRAM words or
direct colors.

The OpenGL rectangle emitter splits forward and reversed traversal at each
byte boundary. Each quad retains the original command's screen position and
the inclusive bounds of its sampled texels. Native rendering rounds texture
coordinates, while scaled rendering floors them. Reversed scaled edges use
the corresponding one-texel endpoint offset so every subpixel in a native
block selects the same texel.

A reversed native quad can end at coordinate -1. Batch vertices carry packed
signed 16-bit coordinates, including that endpoint. Positive polygon and
rectangle coordinates retain their values when decoded by the vertex shader.

## Decoded-source ownership

Each decoded source points to its hash-cache entry and participates in that
entry's source list. Growing the flat hash table relocates its entries. The
cache retargets both the list links and each source's entry pointer before
freeing the preceding table. Source lookup, timestamp updates, invalidation
and destruction then access the current entry.

## Regression coverage

`tests/integration/gpu/hardware_texture_coordinates.c` adds 3,840 rendered
checks to the preceding 692 transfer, palette and decoded-page checks:

- 1,024 rectangle and flat-quad cases cover all 32 page origins, four texture
  modes, 1x/2x and decoded-page cache off/on.
- 768 rectangle cases cover four edge pages, all X/Y flip combinations and
  three texture windows in each mode, scale and cache configuration.
- 2,048 long-rectangle cases cover widths 1, 3, 17, 255, 256, 257, 513 and
  1,023 and heights 1, 3, 17, 255, 256, 257, 509 and 511, with and without
  clipping, in all four modes and flip combinations.

The small cases each draw three source origins. Checks read the actual GL
render target and compare every scaled pixel's native five-bit RGB and mask
bit against independently addressed, immutable input words. Uncovered and
clipped pixels retain the background color. Visible draws must enter the
requested GL batch path; a software mirror cannot satisfy the checks. The
matrix also exercises hash-table growth while decoded sources remain live.

The suite has 4,532 checks. See [commands](mask-history.md#renderer-regression-check).
It does not qualify arbitrary triangle interpolation, fractional vertices,
filtering, shaded or blended texture operations, physical texture-cache
timing, display timing or representative gameplay. RGB8 expansion is outside
the new coordinate assertions; their color contract is native RGB5 plus mask.
