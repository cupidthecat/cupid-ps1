# Partial loads and pending load replacement

`LWL` and `LWR` merge memory bytes into a register without an alignment
exception. A merge immediately after another load to the same register uses
that pending value. It does not retire the first value into the general
register: the instruction following the merge still reads the old register.
An ordinary replacement load cancels the first pending write without merging.
This also applies to moves from COP0 and GTE data/control registers.
A load to another register, or to register zero, leaves the first write active.

Native x86-64 execution keeps the pending register and value available across block
boundaries and memory calls. It cancels a matching incoming write only after a
successful memory access. A fault therefore retains the pending value for
exception entry. Partial merges select their pending input before cancelling
that write; the merged result then receives its own load delay.

## Peripheral read selection

The byte and halfword forms of peripheral accesses use the corresponding read
handler at the selected address. Reading the containing word instead can select the wrong serial
register, consume a CD response outside the requested bytes, and charge for
additional SPU bus strobes. Loads to register zero still perform the read.
RAM fast-memory access retains its aligned word read and existing RAM cost.
On-die word registers and cache control retain their aligned word decoder.
The RAM-size register and cache-control partial-read aliases remain unqualified
against hardware; this correction preserves their existing mapping.

The [CPU byte-access description](https://psx-spx.consoledev.net/dev/ps1/cpu/cpuspecifications/#unaligned-loadstore-details)
defines the selected bytes. The [published memory-access program](https://github.com/JaCzekanski/ps1-tests/blob/5f73d61b27a8d51269b54dde1c456d2d2f7dc865/cpu/access-time/main.cpp)
contains an unaligned SPU word case with a halfword-sized `LWL` followed by a
halfword-sized `LWR`; it must not be interpreted as a single aligned word read.
The retained timing output still differs from that program's published output.
This change does not qualify CPU load overlap or complete physical bus timing.

Three-byte external transfers retain the earlier aligned word path. Their
port mapping and strobe counts need hardware evidence, including the 16-bit
SPU interface. This remains an accuracy gap. Retained hardware results are
compared without tolerances or output normalization. The saved format remains 97.

## Regression coverage

`CpuPendingLoadReplacement` checks all 867 combinations of two load shapes and
same, different or zero destinations in each execution configuration. It
captures both the result and the immediately following instruction's register
read. Five additional runs restore CPU, RAM, CD, SPU, serial and scheduler
records while the first load is pending, immediately before a partial load
or each coprocessor move path. RAM cases include every partial-load offset.

`CpuPartialLoadPorts` compares peripheral values and elapsed emulated ticks
against equivalent byte/halfword loads, including register zero and restored
pending loads. `CpuPartialLoadResponses` checks that selected CD status and
flag reads preserve an unread response.
`CpuPartialLoadWordRegisters` preserves RAM-size and cache-control decoding
across all eight partial-load forms and restored pending loads.

The original generated guest repeats the 867-case matrix and checks live
serial and CD reads in NTSC/PAL across interpreter, cached and linked/unlinked
native slow-memory, LUT and mmap configurations. Each run must make progress
through the selected execution path and reproduce every saved byte after
reset before restoration. A sticky mismatch flag retains errors from the
first instructions after restoration across subsequent matrix iterations.
These software checks establish instruction and
continuation invariants; they do not establish physical-console waveforms or
representative gameplay compatibility.
