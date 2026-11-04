# Shared noise generator

The noise clock runs at 44.1 kHz, independently of voice
pitch. All voices with noise enabled receive the same signed 16-bit level
before their envelopes and volumes are applied.

The six noise-clock bits select a divider shift and a step of 4, 5, 6 or 7.
Divider carries accumulate those steps in a three-bit counter. Its carry
shifts the shared linear feedback shift register (LFSR) once. The four fastest
settings shift once per sample.
At a constant setting, the shift rate is
`min(44100, 44100 * (4 + step_bits) * 2^shift / 131072)`.
This is an LFSR update rate, not a tonal frequency. See the
[noise specification](https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-noise-generator).

Control writes generate elapsed samples with the preceding control value
before applying the new clock. They retain the divider and step-counter
phase. The former fractional accumulator advanced too quickly at many clock
settings; the corrected counter separates the divider from the step carry.

## Save states

Version 92 adds the 16-bit divider and eight-bit step counter before the SPU
transfer balance. Valid divider values are below `8000h`; counter values are
below 8. Invalid or truncated records do not reschedule SPU events.

Earlier states retain their saved LFSR level and legacy accumulator record,
but restart the missing corrected phase at zero. Their old accumulator does
not encode the divider and step counter. Exact new-clock phase continuation
requires version 92. The existing reset seed of 1 remains in use; its physical
reset value and electrical sample phase have not been measured here.

## Checks and limits

```sh
make test TEST_FILTER=SpuNoise
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_spu_noise_live.py
```

Component checks cover all 64 clock rates and first-edge deadlines, clock
changes, shared inputs to voices with different pitches, fractional sample
restoration, legacy records and invalid phase fields. Generated guest programs
check noise levels against sample counts, the saved phase, voice capture data
and strict restored continuation across interpreter, cached and native
slow/LUT/mapped paths with linking enabled and disabled.

These are software regressions checked against documented rates. They do not
establish physical reset seeds, clock-switch electrical timing, complete SPU
accuracy or audible output quality.
