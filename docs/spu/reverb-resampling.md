# Reverb resampling

The reverb input runs at 44.1 kHz and feeds the reflection processor at
22.05 kHz through a 39-tap FIR filter. Its 20 outer nonzero coefficients
multiply input samples at offsets `0, 2, 4, ..., 38`. The centre coefficient
multiplies the sample at offset 19. Zero coefficients contribute nothing.
See the [SPU resampling specification](https://psx-spx.consoledev.net/soundprocessingunitspu/#reverb-buffer-resampling).

The input history is a duplicated 64-sample ring for each channel. The
reflection processor consumes a filtered sample on odd ring positions.
The filter shifts the accumulated products right by 15 and saturates the
result to a signed 16-bit sample before applying the input and reflection
gains. The output filter retains its separate interpolation phases.

The scalar input implementation previously read four adjacent samples,
then skipped four, for each group of coefficients. This placed coefficients
on zero-tap positions and omitted other input samples. Input coefficients
now use their actual two-sample spacing throughout the kernel.

## Checks and limits

```sh
make test TEST_FILTER=SpuReverb
```

The tests inspect reflection samples written to SPU RAM using the full
39-coefficient kernel as an independent expectation. They cover positive
and negative impulses at every retained history position and every active
ring phase, saturation, constant inputs, channel separation and ring wrap.
A generated ADPCM voice supplies changing live samples, including the
newest input tap; the voice capture buffer supplies its pre-volume input
to the expectation.

A continuation case restores CPU, bus, DMA, IRQ, SPU and scheduler state
with partially populated histories. Its later serialized records must
match uninterrupted execution byte for byte, including sound RAM and both
resampling histories. The state layout is unchanged.

These generated checks establish the filter's sample placement and state
continuation. They do not qualify the complete reverb algorithm, physical
RAM arbitration, reverb IRQ timing, analog output or game audio compatibility.
No new physical-console recording was available.
