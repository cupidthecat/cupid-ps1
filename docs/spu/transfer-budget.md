# Transfer tick accounting

The inline transfer model spends 16 CPU ticks per halfword. Each call to
`spu_execute` spends complete halfword budgets once and retains fewer than
16 ticks for the next active slice. A zero or negative slice leaves the SPU
state unchanged. Reaching a full read FIFO or an empty write FIFO discards
unused time; it cannot pay for a later transfer.

DMA requests can synchronously empty or refill the FIFO during a transfer.
Those callbacks share the original elapsed budget. They do not receive a new
budget for each block. Transfer-address IRQ checks follow the halfwords that
actually move.

## State continuation

Save-state version 89 adds a signed 32-bit fractional transfer balance before
the FIFO record. Versions through 88 contain no such balance and load with
zero credit. Their original fractional progress cannot be recovered.

Loading rejects negative balances, balances of 16 ticks or more, FIFO indices
outside the 32-entry ring, oversized FIFO counts and inconsistent ring
positions. An invalid or truncated record does not reschedule SPU events.

## Checks and limits

```sh
make test TEST_FILTER=SpuTransferBudget
```

Generated tests check read and write data, exact consumption within the
existing budget model, zero-time calls, split/batched state equality,
fractional save/load continuation, legacy loading, invalid records, IRQ
addresses and synchronous DMA refill. They need no BIOS or disc.

These checks establish accounting and restoration invariants. They do not
establish a uniform 16-tick physical transfer phase. The transfer path still
runs with the audio batch, and manual writes retain their immediate write
and forced FIFO-drain behavior. Transfer arbitration, register synchronization,
overclock scaling, independent event deadlines and physical IRQ timing need
further qualification. Published RAM-bus observations describe a shared
[SPU state machine](https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-internal-state-machine-from-spu-ram-timing);
they do not prove this inline scheduling model.
