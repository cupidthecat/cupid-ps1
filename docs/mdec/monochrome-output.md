# Monochrome pixel packing

The monochrome converter clips luminance to a signed byte and applies the
unsigned offset when requested. It stores the resulting byte as a zero-extended
value. Four-bit and eight-bit FIFO packing mask each input pixel to that byte,
so a negative sample cannot set bits belonging to neighboring pixels.

The FIFO mask also handles pending pixel records written by a version that
stored sign-extended values. The state layout remains version 92. This does
not repair bytes already packed incorrectly into an older state's output FIFO.

Component regressions use constant and checkerboard blocks with independently
known luma across both decoder routines. They cover signed/unsigned output,
register/DMA reads and writes, pending-copy restoration, partially drained output and legacy
pending pixel records. Four-bit cases use aligned levels; rounding at nibble
boundaries remains unqualified. The generated MDEC guest also checks sixteen
packed words of negative luminance through eight CPU/memory configurations.

```sh
make test TEST_FILTER=MdecMonochrome
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_mdec_commands_live.py
```

No new console capture, complete decoder accuracy or video/gameplay
qualification is claimed. Supplied program/log differences and DMA timeout
diagnostics remain separate from these generated regressions.
