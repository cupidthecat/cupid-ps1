# OpenGL mask history and restored VRAM

A destination pixel with bit 15 set rejects a draw that checks the mask bit.
The OpenGL renderer carries that protection in its depth attachment. Before
reusing depth IDs, it flushes queued geometry and reconstructs depth from the
render target's alpha channel. The color attachment is detached while sampled.

Fills, uploads and copies reconstruct the affected depth region from their
resulting pixels. Setting or clearing bit 15 in uploaded/copied data therefore
changes the protection seen by subsequent rectangles, triangles and lines.

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
```

On Ubuntu, install `xvfb`, `xauth` and `libgl1-mesa-dri` alongside the normal
build dependencies. A working X11 display can be used without `xvfb-run`.

The check creates the actual hardware backend and reads its GL texture through
a temporary framebuffer. Software VRAM cannot satisfy the pixel assertions.
At 1x and 2x it checks rectangles, triangles and lines across forced depth-ID
rollover, uploaded/copied mask data, clearing operations, saved restoration and
resolution changes in both directions. A separate sequence executes 65,535
real draws to exercise natural rollover at each scale. The default matrix has
58 checks, including truncated-state rejection followed by valid restoration.
Setup errors, wrong pixels and GL errors fail the executable.

## Remaining qualification

These cases qualify destination-mask history for the tested operations. They
do not establish full GPU accuracy. Transfer set/check-mask flags, wrapped and
overlapping transfers, textured mask cases, palette/cache interactions, display
timing and representative gameplay still need further work. The existing
software readback path remains separate from these actual GL assertions.
Native Windows OpenGL contexts remain unsupported.
