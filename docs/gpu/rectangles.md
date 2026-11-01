# Rectangle texture coordinates

Textured GP0 rectangles use the horizontal and vertical flip bits in the E1h
draw mode. A horizontal flip begins at `U | 1` and decrements U for each pixel.
A vertical flip decrements V for each row. Coordinates wrap within eight bits
before the texture window is applied. Clipping and skipped interlaced rows
advance the source coordinates by the corresponding distance.

These bits apply to rectangle texture traversal. Untextured rectangles keep
their flat color. Polygon texture coordinates come from their vertex packets.
The draw mode register and queued rectangle words are included in save states;
restoring an incomplete rectangle preserves its texture orientation.
The format remains version 93. Older files retain their recorded draw mode;
a vertical-flip bit already discarded before saving cannot be recovered.

The software scalar and SIMD paths have coordinate, window, clipping, blend,
mask and interlace regression coverage. A generated guest checks indexed and
direct texture reads through GP0/GPUREAD, all CPU execution modes and restored
continuation. These checks do not qualify the separate OpenGL batch emitter,
render timing or every combination on physical hardware.
