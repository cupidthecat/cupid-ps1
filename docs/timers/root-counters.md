# Root counters

The three 16-bit counters select these clock sources:

| Counter | Clock sources |
|---------|---------------|
| Timer 0 | System clock or GPU dot clock |
| Timer 1 | System clock or Hblank |
| Timer 2 | System clock or system clock divided by eight |

Counters wrap from `FFFFh` to zero after 65,536 increments. Target and overflow
comparisons latch separate reached flags. Reading the mode register returns
and clears both flags. Target and overflow events can request one-shot or
repeated interrupts, with pulse or toggle requests.

## Reset phases and clocks

For positive targets below `FFFFh`, reset-at-target mode resets the internal
counter to zero when it reaches the target. The system clock then holds it
for two ticks. During the first hold phase, a counter read returns the target.
After that, reads return zero until counting resumes. For target 10, the
steady sequence is `1, 2, ..., 9, 10, 0, 0, 1, ...`.

GPU clocks and Timer 2's divided clock reset without the system-clock hold.
A zero target is compared on a mode reset or a full wrap. A `FFFFh` target
still permits the next increment to produce an overflow event.

System-clock counter and mode writes add a one-tick hold. Counter and target
writes do not compare the written value immediately. A mode write resets
the counter, clears the interrupt request and rearms one-shot delivery.
These write-edge choices still need broader physical-console coverage.

GPU blanking gates control Timer 0 and Timer 1 synchronization. Timer 2 has
its own sync selections: modes 0 and 3 stop counting; modes 1 and 2 run.
Timer 2 retains the remainder when system ticks do not divide evenly by eight.

Tick updates visit each target or overflow edge. This preserves repeated
requests and toggle parity when an update spans several counter periods.
System-clock scheduling includes pending holds and the divided-clock remainder.
Completed one-shot interrupts do not schedule another request.

## Save states

Save-state version 88 adds one hold byte to each counter record. The timer
payload grows from 59 to 62 bytes. The hold, clock selection, flags, interrupt
latch and divided-clock remainder continue across restoration. Loading a
state from version 87 or earlier defaults the missing hold to zero; its
original phase cannot be recovered. Invalid hold values and counters outside
the 16-bit range are rejected.

## Checks and remaining gaps

Run the generated regressions with:

```sh
make test TEST_FILTER=TimerCounter
```

They cover all three counters, full wraps, readback phases, zero and maximum
targets, Timer 2 sync selections, pulse/toggle and one-shot/repeat requests,
IRQ deadlines, GPU gates, reset state and legacy-state loading. The batching
matrix compares the complete serialized counter and interrupt records and
request counts at 720 checkpoints. A divided-clock case restores CPU, counter
and scheduler state before checking the next interrupt deadline.

The original published timer program still fails its target-readback verdict
under both interpreter and native LUT execution. In the retained run, repeated
reads are 18 system ticks apart and alternate between values 2 and 8 after the
first sample. The generated phase checks pass, but that does not qualify the
program's CPU and bus timing. Its raw histograms and timing differences remain
failures; no tolerance or output substitution is applied.

Pulse requests latch interrupt edges immediately; their physical width is
not modeled. Write-edge comparisons, repeated status reads at the exact
comparison edge, GPU gate timing and clock-domain readback also need further
hardware-backed coverage. No new physical-console capture was available.
The separate modified-clock timer fixture requires motherboard wiring and is
not treated as a stock-console test.
