# GPU reset and interrupts

`GP0(1Fh)` raises the GPU interrupt request in GPUSTAT bit 24 and asserts the
GPU line at the interrupt controller. The controller latches the rising edge
in I_STATUS bit 1. Acknowledging I_STATUS while the GPU line remains asserted
does not create another edge.

`GP1(02h)` clears the GPU request and deasserts its line. `GP1(00h)` resets
command processing and also acknowledges that GPU request. Both commands leave
an interrupt already latched in I_STATUS pending until the CPU acknowledges
it. A later `GP0(1Fh)` can then raise a fresh interrupt. Clearing the command
FIFO with `GP1(01h)` leaves the GPU interrupt request unchanged.

The implementation applies the same interrupt acknowledgement during
`gpu_reset()`, whether or not the caller requests a VRAM clear. The reset
regressions cover an idle FIFO and a partially queued polygon, including
component-state restoration before reset. They also check the mirrored
`GP1(00h)` command bytes `40h`, `80h` and `C0h`.

The `GpuReset` C-test filter checks device registers and generated guest
programs under the interpreter, cached interpreter and native recompiler.
Native cases cover slow memory handlers, mmap and LUT access, with block
linking enabled and disabled. Each guest stores its GPUSTAT and I_STATUS
readbacks in RAM; the test logs all six raw words before checking their IRQ
fields. These cases need no private BIOS or disc.

`GP1(00h)` also resets the VRAM display start address to zero. A previously
selected origin must not survive reset. The display regressions cover four
origins and both initial NTSC/PAL display modes, including restored component
state. Generated guest programs check the same reset under every CPU execution
mode and native memory path. The guest cases compare every named field sent to
the backend's display-update callback. Separate software-rendering cases fill
VRAM with a generated pattern, enable display after reset and compare every
RGBA pixel with a cold-reset frame. They also check the frame before reset
and after component-state restoration. These checks do not verify window
presentation or OpenGL rendering.

The checks cover the reset, display-origin and interrupt sequences above.
They do not qualify
all reset register values, incomplete VRAM transfers, initial beam position,
display timing or physical interrupt latency. No new console capture is
available. The documented reset and acknowledge commands are described in
[PSX-SPX](https://psx-spx.consoledev.net/graphicsprocessingunitgpu/#gp100h-reset-gpu).
