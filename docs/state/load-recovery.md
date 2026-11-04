# Recovery after rejected loads

`system_load_state` parses the file and captures the current complete serialized
machine before changing media or applying records. The backup includes the
actual 2 MiB or 8 MiB RAM configuration. Game path, serial, title, hash, database
entry, console/disc region and taints are retained separately. Incoming taints
are removed when a load is rejected; successful loads retain the existing
taint behavior. The state layout remains version 97.

A different image or playlist sub-image is opened and selected before detaching
the current image. Missing images and invalid indices reject the load. The old
image remains owned by the recovery object until success. Failed loads restore
that same image, its index, metadata and serialized continuation. Successful
loads release the old image once. A saved state without media removes the
current image. Direct CD image replacement also releases the previous owned
image.

Recovery reports the original error. If reinserting the old image or restoring
the backup fails, the error includes the recovery failure and the system shuts
down. It cannot safely continue from partial state. This temporary backup does
not implement a retained user undo action.

## Regression coverage

`StateLoadRecovery` checks rejection without media, with the same media, during
insertion/removal/replacement, and across playlist indices. It checks missing
media, invalid indices, recovery failure after a sidecar changes, RAM size and
taints, supported inner/outer compression, unsupported inner XZ, and image
ownership. Comparisons cover every saved file byte and game metadata.

`StateRecoveryActive` compares valid restoration and rejection while a CPU load
is pending, chopped GPU DMA is paused, a CD command is queued, and timers and
SPU transfers are active. Subsequent execution must deliver interrupts, finish
the GPU transfer, produce the CD response and write sound RAM. Complete future
saved records must match uninterrupted execution across interpreter, cached
interpreter and linked/unlinked native slow, LUT and mmap memory paths.

The generated frontend guest runs active ADPCM/noise voices, sweeps, capture
RAM and reverb in NTSC/PAL across those eight configurations. At frame 3 it
attempts to load a malformed frame-6 state referencing a generated foreign disc.
Both a bad CD record and a bad final marker must report the expected corruption
error. The state immediately after rejection and frame-6 continuation must
match the control run byte for byte. All inputs are generated; these tests
require no private BIOS or game.

```sh
make test TEST_FILTER=StateLoadRecovery
make test TEST_FILTER=StateRecoveryActive
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_state_recovery_live.py
```

The frontend diagnostic uses `CUPID_CHECKPOINT_REJECT_FILE` together with a
checkpoint directory, schedule and restore index. It conflicts with reset
before restore. It requires rejection with unchanged frame/ticks and writes
`rejected-<frame>.sav` for the caller's complete comparison. Normal restore
diagnostics continue using `CUPID_CHECKPOINT_RESTORE` alone.

## Limits

Recovery depends on the current serializers and accessible external resources.
It does not roll back external file changes, host audio/video/input queues,
generated code or asynchronous host reader internals. Loading rebuilds runtime
events, mappings and code caches. The tests cover serialized state and bounded
execution with software rendering, not every malformed input, OpenGL
whole-machine recovery, physical timing, audible/visible output or
representative gameplay.
