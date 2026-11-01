# No-function commands

Command codes 0 and 4 through 7 accept one header word and expect no
parameters. Bits 25 through 28 are reflected to status bits 23 through 26;
the low 16 bits appear in status without subtracting one. These headers leave
busy clear and preserve already decoded output. The next input word starts
another command, even when the header's low bits are nonzero.
See the [MDEC command specification](https://psx-spx.consoledev.net/macroblockdecodermdec/#mdec0-no-function).

The latched no-function state retains its status bits across control writes
and save/load. The state layout remains version 92. Data already discarded
by an older command model cannot be recovered from an older state.

Component checks cover all five codes, all output flags and boundary count
values; subsequent quantization/scale tables; queued DMA input; retained
output through register and DMA reads; and restored continuation. A generated
guest checks status, command boundaries and decoded output in interpreter,
cached and native slow/LUT/mapped execution, with linking enabled and disabled.

```sh
make test TEST_FILTER=MdecNoFunction
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_mdec_commands_live.py
```

This covers command behavior with generated inputs. No new console capture,
exact command latency or complete MDEC/video qualification is claimed. Table
commands issued with unread decoded output remain outside this qualification.
