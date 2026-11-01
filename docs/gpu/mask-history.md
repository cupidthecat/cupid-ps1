# OpenGL mask history and restored VRAM

A destination pixel with bit 15 set rejects a draw that checks the mask bit.
The OpenGL renderer carries that protection in its depth attachment. Before
reusing depth IDs, it flushes queued geometry and reconstructs depth from the
render target's alpha channel. The color attachment is detached while sampled.

Fills, uploads and copies reconstruct the affected depth region from their
resulting pixels. Uploads and copies apply the set-mask and check-mask flags to
each pixel, including transfers across the last VRAM row and column. Checking
the old destination mask protects that pixel even when set-mask is also enabled.
Fills ignore the mask flags.

Transfer pipelines generate a full-screen triangle from the vertex ID and clip
it to the transfer bounds. Uploads read packed halfwords from a buffer or an
integer texture with a nearest sampler. Copies sample the separate VRAM read
texture. The color render target stays separate from its sampled source.

Saved backend state contains canonical VRAM and CLUT data. Loading uploads that
VRAM into the live color texture and reconstructs its mask depth immediately.
Runtime resolution changes also restore canonical pixels after recreating the
textures. Crossing between native and scaled rendering rebuilds the batch
shaders that use different pixel centers. The saved record format is unchanged.

## Renderer regression check

```sh
make -j2 gpu-gl-tests gl-smoke
SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/cupid-ps1-gpu-gl-tests
SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=3.3CORE \
  MESA_GLSL_VERSION_OVERRIDE=330 xvfb-run -a ./build/cupid-ps1-gpu-gl-tests
SDL_VIDEODRIVER=x11 LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a ./build/cupid-ps1-gpu-gl-tests --no-texture-buffers
```

On Ubuntu, install `xvfb`, `xauth` and `libgl1-mesa-dri` alongside the normal
build dependencies. A working X11 display can be used without `xvfb-run`.

The check creates the actual hardware backend and reads its GL texture through
a temporary framebuffer. Software VRAM cannot satisfy the pixel assertions.
At 1x and 2x it checks rectangles, triangles and lines across forced depth-ID
rollover, uploaded/copied mask data, clearing operations, saved restoration and
resolution changes in both directions. A separate sequence executes 65,535
real draws to exercise natural rollover at each scale. The default matrix has
4,532 checks, including truncated-state rejection followed by valid restoration.
The transfer cases exercise set-mask, check-mask and both flags together. Wrapped
2x2 uploads and copies check all four corners across both VRAM edges, then issue
protected draws to those destinations. A wrapped source is also copied to an
ordinary destination.

A 257x256 grid contains every possible 16-bit pixel value and uses an odd source
row pitch. Upload and copy checks compare every scaled RGBA pixel exactly,
including mask alpha. A rectangular three-level host texture checks source
advancement and per-level row stride without reading outside its guarded input.
The capability override runs the same matrix through the
integer-texture upload path. The [palette-cache matrix](palette-cache.md) adds
64 checks of cached colors, draw ordering, rejected state reads and restoration.
The [decoded-page matrix](texture-pages.md) adds 512 checks across every page
origin and texture mode, before and after a source-word rewrite.
The [rendered-coordinate matrix](texture-coordinates.md) adds 3,840 checks of
rectangles, flat quads, texture windows, flips, long traversal and clipping.
CI checks both upload paths under each compiler
with default Mesa and forced OpenGL 3.3. Setup errors, wrong pixels and GL errors
fail the executable.

## Remaining qualification

These cases qualify destination-mask history for the tested operations. They
do not establish full GPU accuracy. Overlapping-copy ordering, other transfer
sizes and interlaced fills, textured mask cases, palette/cache interactions,
display timing and representative gameplay still need further work. The existing
software readback path remains separate from these actual GL assertions.
Native Windows OpenGL contexts remain unsupported.
