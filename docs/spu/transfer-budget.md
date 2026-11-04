# Transfer tick accounting

The transfer model spends 16 CPU ticks per halfword. A dedicated scheduler
event advances it independently of the audio batch, so DMA-register polling
does not depend on a later SPU status read. Each callback spends complete
halfword budgets once and retains fewer than 16 ticks for the next slice.
Reaching a full read FIFO or an empty write FIFO cancels the event and
discards unused time; it cannot pay for a later transfer.

DMA requests can synchronously empty or refill the FIFO during a transfer.
Those callbacks share the original elapsed budget. They do not receive a new
budget for each block. Transfer-address IRQ checks follow the halfwords that
actually move. The next event is one halfword away, less any fractional
credit. Pending transfers also synchronize before status/control accesses or
an address change. Changing the address preserves the unconsumed FIFO data.

Audio has a separate elapsed-time cursor. Before a RAM transfer, pending
samples consume their older RAM inputs. An audio callback does not also
spend transfer time.

## State continuation

Save-state version 90 records the independent transfer event in the shared
event queue. Version 89 adds a signed 32-bit fractional transfer balance
before the FIFO record. Versions through 88 contain no balance and load with
zero credit. Their original fractional progress cannot be recovered.

Legacy streams have no transfer-event record. After the saved global clock
and CPU state load, pending work receives a new deadline using the restored
fractional credit, if present. The loader does not schedule against the clock
of the session being replaced.

Loading rejects negative balances, balances of 16 ticks or more, FIFO indices
outside the 32-entry ring, oversized FIFO counts and inconsistent ring
positions. An invalid or truncated record does not reschedule SPU events.

## Checks and limits

```sh
make test TEST_FILTER=SpuTransferBudget
make test TEST_FILTER=SpuTransferEvents
make test TEST_FILTER=SpuTransferTimeline
```

Generated tests check read and write data, exact consumption within the
existing budget model, zero-time calls, split/batched state equality,
fractional save/load continuation, legacy loading, invalid records, IRQ
addresses and synchronous DMA refill. Guest programs poll DMA completion
across interpreter, cached and native execution, slow/mmap/LUT accesses and
linking settings. Mid-transfer restoration compares every serialized byte
of CPU, RAM/bus, DMA, IRQ, SPU and scheduler records before continuing.
Timeline cases check audio capture ordering, address changes, reset, legacy
deadlines and IRQ delivery without status polling. They need no BIOS or disc.

These checks establish accounting and restoration invariants. They do not
establish a uniform 16-tick physical transfer phase. Manual writes retain
their immediate write and forced FIFO-drain behavior. Transfer arbitration,
mode-change edges, overclock scaling, voice/capture/reverb IRQ timing and
console-revision differences need further qualification. Published RAM-bus
observations describe a shared
[SPU state machine](https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-internal-state-machine-from-spu-ram-timing);
they do not prove this scheduling model.
