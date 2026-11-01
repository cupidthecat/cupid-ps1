# Reset status and cancellation

A power or control reset leaves the status register at `80040000h` when DMA
requests are disabled. Its low 16-bit word counter is zero. An idle counter
can hold this value without expecting a parameter word. See the
[MDEC register specification](https://psx-spx.consoledev.net/macroblockdecodermdec/).

Reset clears queued input/output, header format flags and the current decode
phase, and cancels pending block copy-out. Changing DMA enables afterward
retains the zero counter until a new command loads it. Completed commands
still leave their normal `FFFFh` counter value. Exact DMA request/arbitration
phase and command latency remain unqualified.

The existing idle counter encoding stores halfwords with a one-word bias.
Reset initializes that representation to two halfwords; this is register
state, not queued data. The state layout stays at version 92. Older states
retain their saved count until an explicit reset.

Component regressions cover power/reset status, control rewrites, cancellation
with queued input and pending copy-out, discarded partially read output and
state restoration across both decoder routines. The generated MDEC guest
also reads reset status through eight CPU/memory configurations before
checking commands and pixel output.

```sh
make test TEST_FILTER=MdecReset
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_mdec_commands_live.py
```
