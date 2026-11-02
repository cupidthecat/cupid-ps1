# CD saved-state records

CD records contain bounded parameter, response and asynchronous-response queues,
eight sector buffers, XA predictor and resampler history, and a stereo audio
FIFO. The current saved format remains version 96.

The loader rejects a queue count beyond its capacity or a payload that does
not fit in the remaining record. It does not clamp a serialized count and then
continue at a different cursor. In particular, an oversized audio count must
not leave an audio frame to be interpreted as the requested sector address.
The full 88,200-frame FIFO remains valid.

Sector buffer selectors must be below eight. Each sector's declared length
must fit its 2,340-byte array, and its cursor must not exceed that length.
These checks run before a variable payload is copied into the sector buffer.
The legacy fixed-buffer representation also checks selectors and lengths.

XA ring positions must fit the 32-sample history. The half-rate interpolation
path subtracts seven once before indexing one of seven coefficient rows, so a
saved counter above 13 cannot safely reach that path. Bounded startup counters
are retained without normalization. This bound does not establish the physical
meaning or timing of every startup phase.

A wrapper that already has an error returns immediately. Reads stage the CD
controller and nonempty audio queue in separate storage. Only a complete,
validated record replaces the live controller. A rejected CD record leaves
the controller, queued audio, pending media read and event schedule unchanged.
This guarantee covers the CD component; the full machine loader can already
have restored other components before reaching a bad CD record.

Writing an older format preserves live timestamps and sector cursors, even if
the write fails. Loading versions 42 through 64 remains supported; exporting
their obsolete copied-FIFO representation is not supported.

A separate generated case still loses an active command event when restoring
after a data sector arrives during a pending command. The record checks below
do not qualify that overlap; command-event restoration needs further work.

## Regression checks

```sh
make test TEST_FILTER=CdStateRecord
make test TEST_FILTER=CdLegacyRecord
make test TEST_FILTER=CdXaContinuation
```

The generated cases exercise fifteen format-version boundaries from 65 through
96. They check oversized counts, invalid sector metadata, ring positions,
interpolation counters, every truncated prefix of an empty record, sticky read
and write errors, full-capacity audio, and records at a nonzero wrapper offset.
After each rejected read, every serialized CD byte must remain unchanged,
before a valid record is restored. The capacity case reads and checks all
88,200 stereo frames. Older-format writes must leave the current record intact.

Independent fixture writers cover every fixed-buffer layout version from 42
through 64. They exercise empty, partial and full legacy data FIFOs, all eight
sector buffers, queued stereo frames, invalid metadata and truncated payloads.
The fixtures do not use the production legacy writer. Converted records must
round-trip through the current format with every serialized byte preserved.
These checks preserve the existing legacy FIFO transplant; they do not claim
that every external historical state has been reproduced.

## XA continuation

Generated Mode 2 sectors travel through SetMode, ReadN, the image reader, XA
decoding and the CD audio queue. Four cases cover 4-bit stereo, 8-bit stereo,
4-bit mono at half rate, and 8-bit stereo at half rate. Payloads vary all sample
bits and use the four defined predictors. Other interleaved sectors use a
different channel.

The checks restore before ReadN acknowledgement, between selected sectors and
after consuming part of a queued packet. They compare every replayed sample and
every serialized CPU, interrupt, CD and scheduler byte. No sample offsets or
state fields are normalized. Half-rate packet lengths are read from the saved
queue, and complete output streams are compared without trimming startup data.
A rejected CD record during a queued packet must preserve that complete state
and the remaining samples.

The fixture pauses the SPU consumer and reads the queue explicitly. Every
serialized SPU byte must remain unchanged, so SPU restoration is unnecessary in
this isolated test. No guest instructions execute in this fixture. Physical CD
timing, complete decoder and resampler arithmetic, native execution during XA
playback, malformed audio sectors, audible playback and representative gameplay
remain unqualified.
