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
failed read does not convert an incomplete matrix. Current state layout
version 92 is unchanged.

Regressions reject out-of-range fields and a copy event paired with an idle
state, check truncation in current/legacy layouts, and verify that legacy
matrix writes preserve live state and round-trip the table.

```sh
make test TEST_FILTER=MdecState
```
