# GTE arithmetic

Color output uses the signed MAC1, MAC2 and MAC3 values after an arithmetic
right shift by four. Each channel then clips to 0 through 255 and sets its
color saturation flag when clipping occurs. For example, MAC = -1 remains
negative after the shift: its output is zero and its saturation flag is set.
The CODE byte comes from RGBC. Single-color commands advance the RGB FIFO
once; triple-color commands advance it three times.

Translation, far-color, IR and accumulated MAC values use signed fixed-point
scaling. Multiplication by the appropriate power of two keeps negative
inputs defined in C11. These products fit in their chosen intermediate
types; the existing 44-bit chained-sum and 32-bit result rules still apply.
Stored MAC values wrap to their low 32 bits rather than saturating.

The save-state format remains version 93. Loading retains saved FIFO values
and pending deadlines; it does not reconstruct results produced by older
emulation.

INTPL reports eight command cycles. The other supported command counts
follow the [published GTE command table](https://psx-spx.consoledev.net/geometrytransformationenginegte/#gte-opcode-summary).
Checking these numbers does not establish physical issue, completion or
CPU stall timing.

## Checks

`make test TEST_FILTER=Gte` runs arithmetic, FIFO, FLAG, state and CPU
interaction regressions. Directed cases include negative and positive color
boundaries, both shift and lower-limit settings, all fourteen color commands,
negative translation and projection, and all twenty-two supported command
counts. CPU cases check result-read load delays and restoration while a
command is pending across interpreter, cached and native slow/mmap/LUT paths,
including disabled linking.

The generated firmware test is `test_gte_color_live.py`. Its 144 cases check
color boundaries, command pipelines and signed translations through actual
coprocessor instructions. It compares complete uninterrupted and restored
states at two frame boundaries, retaining timing fields and CPU bookkeeping.
The firmware and executable are generated locally without private inputs.

A separate supplied vector program checks all 64 readable registers in 50
register-only cases and 1,100 command cases. Its table matches the supplied
capture data. Interpreter and configured native LUT runs on Linux and Windows
report 1,150 passes and zero failures. The native runs include ordinary
interpreted instructions; the generated guest separately requires native
progress without ordinary fallback. The supplied program checks data results,
not command timing, and its inputs remain outside public CI.

Undefined commands, possible aliases, physical pipeline phase and complete
hardware arithmetic coverage remain unqualified. These software regressions
do not establish visual compatibility, audible playback or gameplay accuracy.
