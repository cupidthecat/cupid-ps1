# BIOS and boot

- Required. The emulator exits with an error if no BIOS is found.
- `--bios PATH` overrides; otherwise looks for `./SCPH1001.BIN` then `./SCPH7003.bin` in the working directory.
- Standard 512 KB image, mapped at `0x1FC00000`.
- MD5 is computed and looked up in a 44-entry known-BIOS table (`src/core/bios.c`); unknown hashes load with a warning.
- Region detection (NTSC-U/J, PAL) feeds disc-speed and timing assumptions.
- Fast boot is gated on `CUPID_FASTBOOT=1`; off by default to avoid black screens on titles like Crash Bandicoot.
