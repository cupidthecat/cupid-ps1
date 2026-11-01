# GPU information queries

GP1 commands `10h` through `1Fh` select information for the GPUREAD latch.
The low four parameter bits select the item; higher parameter bits do not
change that selection.

| Index | Latched value |
|-------|---------------|
| `0`, `1`, `6`, `9` through `F` | Keep the previous value |
| `2` | Texture window |
| `3` | Drawing-area top left |
| `4` | Drawing-area bottom right |
| `5` | Drawing offset, with each signed coordinate encoded in 11 bits |
| `7` | `2`, the emulated v2 GPU revision |
| `8` | `0` |

Changing a drawing register through GP0 does not refresh an information value
already held in GPUREAD. Repeated reads return the latched value. Information
reads do not require a VRAM-read packet, GPUSTAT bit 27 or a DMA request.

The existing GPU state record stores the read latch. Restoring it preserves
the value independently of later drawing-register writes or unknown-index
queries. This behavior requires no state-format change.

## Regression coverage

`tests/core/gpu/test_gpu_information.c` exercises all 512 combinations of the
16 GP1 command mirrors and 32 parameter indices, with nonzero upper parameter
bits. It also checks repeated reads, DMA-read status without a VRAM request,
and a saved latch restored after another query changes it. Setup drains the
scheduled GPU command events before querying drawing registers.

The original generated program in
`tests/integration/gpu/information_program.py` repeats those 512 queries through
guest MMIO. Each row records the complete query word, the preceding latch and
three later reads, including a read after another GP0 register write. The live
test checks all recorded words without masking them. It saves the revision
reply at frame 3, resets before restoring on one side, and requires the full
matrix to finish by frame 9. Both continuous and restored execution run under
the interpreter, cached interpreter, and linked/unlinked recompiler with
fast memory off, LUT and mmap. The same guest runs in native Windows CI.

These checks cover v2 information selection and saved continuation. Older GPU
revisions, unused upper readback bits and interactions with an active VRAM read
remain separate accuracy questions. No physical-console capture is included.
