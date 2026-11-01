# Decoded texture-page wrapping

The OpenGL decoded-page cache reads each native VRAM row independently.
Texture coordinates select a word relative to the page origin, then wrap its
X coordinate to 0..1023 before reading it. Crossing the right edge keeps the
same Y coordinate. Four-bit indices use four texels per word, eight-bit indices
use two, and direct colors use one.

Page hashes cover the same words in the same order as decoding. When a row
wraps, the hash includes its right-hand tail followed by the beginning of that
row. A write to the wrapped portion invalidates the decoded source and changes
its hash, so lookup cannot reuse a texture containing the preceding colors.
The source-page references already track the pages on both sides of the edge.

## Regression coverage

`tests/integration/gpu/hardware_texture_pages.c` checks all 32 page origins in
four-bit, eight-bit, direct and reserved-direct modes at 1x and 2x. Each case
reads the complete 256x256 decoded GL texture and compares every RGBA8 pixel
against independently addressed native VRAM and the latched palette. The host
RGBA8 expectation rounds each native five-bit channel to 0..255.

For each page, the test rewrites its final source word through the backend and
checks the complete decoded texture again. The wrapped cases therefore check
both initial decoding and invalidation/hash reuse. The lower-half pages include
the final VRAM row. These 512 checks run in `gpu-gl-tests` alongside the
[rendered-coordinate matrix](texture-coordinates.md). See
[commands](mask-history.md#renderer-regression-check).

This matrix checks decoded cache contents. It does not qualify every textured
primitive's UV interpolation, texture-window behavior, filtering, replacement
textures, physical cache eviction or cache timing. Those require separate
rendering and hardware evidence.
