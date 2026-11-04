# Output order and DMA placement

Colored output reaches the CPU data port as four consecutive 8 by 8 tiles:
top left, top right, bottom left and bottom right. DMA channel 1 places the
same words into a 16 by 16 raster in RAM. A tile row occupies four words
for 15-bit pixels or six words for 24-bit pixels. Monochrome output remains
linear. The [MDEC format documentation](https://psx-spx.consoledev.net/macroblockdecodermdec/)
describes this distinction.

The decoder retains its raster working buffer and packs the output FIFO
in tile order. DMA applies the signed row offset to each destination word,
then masks the resulting RAM address. Address wrapping and code-cache
invalidation use that actual destination. Only DMA reads advance the placement
cursor; CPU reads consume output without advancing it. A new decode command
resets the cursor. No-function headers retain the queued layout even when
their reported depth differs. The cursor continues across macroblocks in
the same decode command and survives save/load.

Component regressions check every output byte for both color depths,
signed output, bit 15 and both decoder routines. They exercise real DMA
bursts, split and wrapped transfers, compiled-page invalidation, mixed CPU
and DMA reads, multiple macroblocks, block status, and old queued records.

The original generated integration guest checks 13 cases in each of eight
CPU configurations, with both decoder routines. A rounding control confirms
which routine actually ran. Its first checkpoint retains 124 of 128 words
after a four-word DMA burst. The second checkpoint checks all completed
pixel captures. An uninterrupted run and a restored run must match every
serialized byte. The same guest runs on native Windows.

```sh
make test TEST_JOBS=4 TEST_FILTER=MdecSpatial
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest tests.integration.mdec.test_mdec_spatial_live
```

These are generated-input and component checks. They do not qualify physical
FIFO capacity, DMA request thresholds, per-block latency, conversion precision,
commercial FMV playback or complete MDEC hardware behavior. No physical-console
capture was made for this change.
