# Executable entry registers

When loading a PS-X EXE for execution, cupid-ps1 sets PC and GP from the
header. A nonzero stack base sets both SP and FP to the 32-bit sum of the
base and offset, including when that sum wraps to zero. A zero stack base
preserves the existing SP and FP and ignores the offset. Loading without
entering the executable preserves these registers while still uploading
the payload and clearing the specified memory region.

This stack rule follows the [PS-X EXE header description](https://psx-spx.consoledev.net/cdromfileformats/#cdrom-file-playstation-exe-and-systemcnf)
and [BIOS Exec interface](https://psx-spx.consoledev.net/kernelbios/#a43h-execheaderbuf-param1-param2).
A local generated guest also called the existing BIOS Exec routine with
zero bases, nonzero offsets, ordinary stacks and a sum that wraps to zero.
Its entry and restored caller registers agreed across independent emulated
CPU cores. This experiment used a separately supplied BIOS; it was not a
physical-console capture.

The component tests cover entry registers, payload loading, memory clearing
and load-only behavior. The generated integration test supplies its own
firmware, reaches the executable injection hook with known registers, and
checks the loaded program's observations. It covers interpretation, cached
interpretation and native execution with helper, LUT and mapped memory
accesses, block linking enabled or disabled, and save-state continuation.
These generated tests require no proprietary BIOS or game files.
