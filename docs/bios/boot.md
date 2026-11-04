# BIOS and boot

The emulator requires a BIOS and exits with an error if it cannot find one.
Pass its path with `--bios PATH`. Without that option, the frontend looks for
`./SCPH1001.BIN`, then `./SCPH7003.bin` in the working directory.

A standard BIOS image is 512 KB and maps to `0x1FC00000`. The core computes its
MD5 and checks the 44-entry BIOS table in `src/core/bios/bios.c`. Unknown hashes
load with a warning. Region detection (NTSC-U/J or PAL) supplies the disc-speed
and timing assumptions.

Set `CUPID_FASTBOOT=1` to enable fast boot. It is off by default because it can
cause black screens in titles such as Crash Bandicoot.
