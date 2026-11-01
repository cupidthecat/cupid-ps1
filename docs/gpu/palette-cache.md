# Palette cache in the OpenGL renderer

Palette loading reads native VRAM into the latched 16-bit color table. The
OpenGL backend uploads that table to a separate, unscaled 256-entry texture.
Loading a palette does not change VRAM pixels, mask depth or VRAM dirty regions.
Rewriting the palette's source VRAM therefore leaves the latched colors in use
until the next palette-load command.

Paletted batch shaders fetch that private table by index. Decoded-page textures
also use the latched table and hash all 16 or 256 applicable entries. An 8-bit
palette that crosses the last VRAM column includes the wrapped entries in its
cache key. Packed texture indices and decoded pages use explicit nearest
sampling; the renderer's shader filter controls remain separate.

Queued geometry is flushed before changing the table or discarding decoded
pages. This keeps an earlier draw's palette and texture alive until that draw
has consumed them.

## Saved continuation and resource recreation

Backend records retain native VRAM followed by the latched palette, with their
existing byte layout. A shared helper checks the complete backend record before
copying either buffer. Software and OpenGL backends both use it. A truncated
record leaves these two canonical buffers unchanged. Valid OpenGL loads upload
VRAM and the saved palette separately, so a saved palette may differ from its
source VRAM.

Resolution changes recreate the private palette from the latched values.
Changing scale does not reload it from VRAM. These guarantees cover this GPU
backend record; they do not establish a transaction across every emulator
subsystem.

## Regression coverage

The standalone `gpu-gl-tests` executable includes palette checks in
`tests/integration/gpu/hardware_clut.c`. The matrix uses 4-bit and 8-bit textures,
at 1x and 2x, with the decoded-page cache enabled and disabled. Each of those
eight configurations checks:

- Unchanged rendered VRAM across palette loading, including the entire source row.
- Retained colors after source writes and refreshed colors after an explicit reload.
- Draw ordering across palette changes and cache clearing.
- Saved palette values that differ from VRAM, plus five truncated-record boundaries.
- Saved colors across resolution changes in both directions.
- The final palette index, including a wrapped 8-bit tail update and reload.
- Transparent zero, opaque black with bit 15 set, and destination-mask protection.

These 64 palette cases run alongside transfer, depth, mip, continuation,
[decoded-page](texture-pages.md) and [rendered-coordinate](texture-coordinates.md)
checks. Assertions read actual GL textures. Canonical-buffer byte
comparisons additionally check rejected state reads. Every run requires the
requested decoded-page-cache setting and checks that textured batches use the
expected GL path.

The `GpuBackendState` C-test filter separately checks the software backend's
wire layout, complete byte restoration, rejected VRAM/palette prefixes, sticky
errors, invalid cursors, failed writes and nonzero record offsets under both
wrapper versions 85 and 96. The backend record itself has no version fields.
Six methods add these checks to the component suite.

See [GPU regression commands](mask-history.md#renderer-regression-check).
CI runs all three compiler profiles with default Mesa and forced OpenGL 3.3,
using both buffer and integer-texture uploads. This tests twelve combinations.

Full texture-cache hardware behavior, textured-primitive UV interpolation and
filter modes, overlap ordering, interlaced rendering, display timing and representative
gameplay remain unqualified. The palette tests do not change the command
processor's palette-address invalidation or mode-transition timing rules.
