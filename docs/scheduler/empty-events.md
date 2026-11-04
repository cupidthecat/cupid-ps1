# Empty event lists

Component reset and state restoration can leave the scheduler without an
active event. Countdown updates and callback dispatch must handle that
state, including when a callback removes the last event.

With no active event, the CPU countdown uses `UINT32_MAX` to represent the
absence of a device deadline. A pending interrupt still sets the countdown
to zero. Activating the next event replaces the idle value with that
event's deadline. Committing CPU time advances the scheduler clocks even
when there are no callbacks to run.

The `TimingEventEmpty` C-test filter checks direct countdown updates,
current-state round trips and empty legacy states at versions 42, 43 and
70. It also checks pending software interrupts, removal of the last event,
and scheduling the first event after an empty restore. Generated RAM
programs run until their observer callback removes itself. The callback
must fire once at its scheduled time and leave the list empty.

These checks cover scheduler lifecycle and serialization. They keep
finite event deadlines and the save-state format unchanged. They do not
qualify physical timer, DMA or display timing.
