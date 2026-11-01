# State record validation

MDEC loads check FIFO indices/counts, decoder state, block/coefficient indices,
quantization scale and the encoded word-counter range before activating
copy-out. A pending copy requires the writing phase. Invalid or truncated
records leave that event inactive and return a load error.

This is a component boundary check. Fields may already have been read into
the component when loading fails; it does not make the entire system load
transactional or qualify every possible malformed record.

Records before version 66 store the scale matrix as raw column-major values.
The adapter converts that representation on load and writes its inverse
without changing the live matrix. Temporary values are initialized, and a
failed read does not convert an incomplete matrix.

Version 93 appends two bytes after the pending-copy flag: the colored
DMA row width in words (4 or 6, with zero for monochrome or legacy queues)
and the DMA word cursor within a macroblock. Loads reject invalid widths,
cursors and queue sizes.

Regressions reject out-of-range fields and a copy event paired with an idle
state, check truncation in current/legacy layouts, and verify that legacy
matrix writes preserve live state and round-trip the table.

Version-92 and earlier queued pixels retain their saved raster order and
use linear DMA placement until the queue drains. Newly decoded colored
pixels use tiled device order, including a pending copy restored from an
older record. Genuine version-92 full and partial queues are retained as
[generated fixtures](../../tests/core/mdec/fixtures/README.md). Writing a
legacy record fails when a live tiled queue or DMA cursor would be lost.

See [output order and DMA placement](output-order.md) for split-transfer
and restored-continuation coverage.

```sh
make test TEST_FILTER=MdecState
```
