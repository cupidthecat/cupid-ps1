# Executable loading

When loading a PS-X EXE for execution, cupid-ps1 sets PC and GP from the
header. A nonzero stack base sets both SP and FP to the 32-bit sum of the
base and offset, including when that sum wraps to zero. A zero stack base
preserves the existing SP and FP and ignores the offset. Loading without
entering the executable preserves these registers while still uploading
the payload and then clearing the specified memory region.

Memory initialization follows payload upload, so a fill range that overlaps
loaded bytes clears those bytes too. A range can extend beyond the payload;
bytes outside the range remain unchanged. A zero fill size preserves the
uploaded bytes. This follows the BIOS Load/Exec sequence: loading writes the
file into RAM, then Exec clears the initialization region before entering
the program. The fill regressions use word-aligned addresses and sizes.

This stack rule follows the [PS-X EXE header description](https://psx-spx.consoledev.net/cdromfileformats/#cdrom-file-playstation-exe-and-systemcnf)
and [BIOS Exec interface](https://psx-spx.consoledev.net/kernelbios/#a43h-execheaderbuf-param1-param2).
A local generated guest also called the existing BIOS Exec routine with
zero bases, nonzero offsets, ordinary stacks and a sum that wraps to zero.
Its entry and restored caller registers agreed across independent emulated
CPU cores. This experiment used a separately supplied BIOS; it was not a
physical-console capture. Further aligned memory-fill calls confirmed the
cleared bytes and preserved surrounding words across the same cores.

The component tests cover entry registers, payload loading, overlapping and
extended fill ranges, zero sizes and load-only behavior. The generated
integration tests supply their own firmware, reach the executable injection
hook, and check the loaded programs' register and memory observations.
They cover interpretation, cached interpretation and native execution with
helper, LUT and mapped memory accesses, block linking enabled or disabled,
and save-state continuation.
These generated tests require no proprietary BIOS or game files.
