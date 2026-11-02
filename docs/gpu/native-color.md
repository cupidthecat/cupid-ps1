# Native color and primitive ordering

The OpenGL renderer's native color path quantizes channels to five bits before
semitransparency. For background `B` and foreground `F`, each in `0..31`, the four
modes produce:

| Mode | Channel result |
| --- | --- |
| Average | `floor((B + F) / 2)` |
| Add | `min(B + F, 31)` |
| Subtract | `max(B - F, 0)` |
| Quarter foreground | `min(B + floor(F / 4), 31)` |

Textured primitives blend only texels with bit 15 set when the command enables
semitransparency. Other nonzero texels overwrite the destination. A zero texture
word discards the pixel; `0x8000` is a visible black texel. Texture bit 15 and the
force-mask flag determine the written mask. Checking the old destination mask
protects that pixel before a write.

Nearest-filtered native draws sample the separate GPU VRAM read texture for
their background and perform the arithmetic in the fragment shader. Host
blending is disabled for this path. It uses neither a framebuffer feedback
loop nor a CPU upload to supply expected pixels. Before each such primitive,
queued geometry drains so the next background copy includes preceding writes.
Quads submit their two triangles in order, and line pairs submit their segments
in order. A conservative drawing-area dirty region determines the read-texture
copy. These copies can cost host time; they add no emulated GPU ticks.

Command dithering uses the native 4x4 matrix before truncation. Raw textures
bypass modulation and dithering. Native draws without command dithering have a
separate pipeline row. Batch comparisons use the same effective dither flag as
the latched pipeline state, including transitions between modulated and raw
textures. The existing scaled-dither option places the pattern in physical
pixels; ordinary unscaled dithering uses native VRAM coordinates at both 1x and
2x resolution.

The shader uniform block is 48 bytes, with the transparency mode at byte 44.
Compile-time checks verify this layout. CPU execution, saved records and default
settings are unchanged. True color and filtered rendering retain their existing
enhanced arithmetic and are outside this native blending qualification.

## Generated regression inputs

`make gpu-gl-tests` builds a check that reads the actual GL color attachment.
It selects native color explicitly. `--true-color` runs the existing renderer
checks in true color; the native-specific groups still create and assert native
backends themselves. Both options run the complete 7,116 checks:

| Group | Checks |
| --- | ---: |
| Mask, transfer, palette, decoded pages and rendered coordinates | 4,532 |
| Untextured blend-pair atlas | 8 |
| Textured/untextured blend and mask atlas | 1,440 |
| Command dithering and raw-texture bypass | 144 |
| Zero, masked black and other special texture words | 320 |
| Scaled-dither coordinates | 16 |
| Sequential, quad-triangle and line-segment overlap | 640 |
| Queued modulated/raw dither transitions | 16 |

Each blend atlas covers all 1,024 foreground/background pairs per channel, using
independent channel permutations. Pixel checks inspect every scaled subpixel
and the mask alpha. They cover 1x/2x rendering, decoded-cache on/off, four texture
modes, rectangles/quads, opaque and semitransparent commands, and all set/check
mask combinations. Overlap cases inspect selected interior points with known
coverage; they do not establish complete line rasterization accuracy.

CI runs GCC, Clang and address/undefined-behavior sanitizer builds, each with
native/true-color selection, default/forced OpenGL 3.3 contexts, and buffer or
integer-texture uploads. Separate graphics and Python jobs restore the exact
compiler artifacts and verify their revision and hashes. All jobs retain their
20-minute limit. Inputs are generated; the checks need no BIOS or commercial
disc.

## Remaining qualification

These cases cover nearest-filtered native color, flat integer-aligned fixtures
and the stated ordering cases. Arbitrary triangles, fractional coverage, Gouraud
interpolation, non-neutral modulation, overlapping texture feedback, hardware
texture-cache timing, display timing and representative gameplay need further
qualification. Passing these checks does not establish full GPU or game accuracy.
Native Windows OpenGL contexts remain unsupported.
