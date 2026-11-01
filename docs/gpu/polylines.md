# Polyline packets and state

Polyline parsing consumes two complete starting vertices before recognizing
the terminator. This takes three words for flat color and four for shaded
color. Later shaded vertices contain a color followed by a position; a color
waiting for its position remains part of the pending packet.

Submission splits complete segments into bounded backend batches. Each batch
starts with the preceding endpoint and its color. Integer and precise paths
submit all segments. When staging fills, complete segments are submitted and
the shared endpoint, source address and any incomplete color are retained.
This keeps memory bounded without discarding vertices or blocking later FIFO
words. Reset and GP1 command-buffer clear discard pending context.

Save-state version 94 stores the staged 64-bit words, including source
addresses used by precise vertices. Loads reject out-of-range FIFO, transfer
and polyline counts, inconsistent pending context and truncated data. Failed
GPU queue loads cancel that queue; they do not roll back an entire system load.
Older idle records remain readable. Older pending polylines cannot be restored
because their vertices were absent from the saved stream.

The version-93 fixtures contain GPU-only records written by revision `f391d01`
from an original synthetic guest. Their compressed bytes, original lengths and
hashes are checked alongside the exact stimulus inputs. They contain no BIOS,
RAM or save-container data, and their headers were not patched to impersonate
an earlier writer.

Component checks cover minimum packets, reset, actual clobber/restore,
shaded color carry, source-address preservation, staging boundaries, malformed
records and old records. A generated guest checks VRAM readback and complete
continuation after resetting the emulator before loading its saved state in
all eight CPU/memory/linking configurations.

These checks do not qualify physical drawing timing or when each partial
segment becomes visible. Small pending primitives still wait for a terminator;
full staging prefixes submit earlier. Precise-path checks cover native
coordinates and fallback depth; enhanced projection and the separate OpenGL
path require further qualification.
