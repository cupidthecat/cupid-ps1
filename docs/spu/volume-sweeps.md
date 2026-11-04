# Volume controls and sweeps

Each voice and the main stereo output have separate left/right gains. The
current voice registers at `1F801E00h + 4*N` and `+2` report left and right,
respectively. Main current gains are at `1F801DB8h` and `1F801DBAh`.

Fixed-volume writes change the control immediately and apply their gain on
the next sample clock. Sweeps also clock once per sample, including idle
voices. Decreasing sweeps continue toward zero; reaching zero stops them.
Linear negative-phase decreases approach zero from below. Exponential
decreases ignore the phase bit. After exponential slowing, a non-frozen rate
retains at least one counter increment; the all-ones rate stays frozen.
See the [volume and envelope specification](https://psx-spx.consoledev.net/soundprocessingunitspu/#spu-volume-and-adsr-generator).

State records already contain both control and current gain, so a pending
fixed gain needs no extra format field. Loading repairs an old inactive-sweep
flag when its gain has not reached the endpoint. Writing a main current gain
can likewise resume a finished sweep. These repairs retain counter phase and
leave intentionally frozen rates inactive.

Loads reject invalid envelope flags and out-of-range rates or counters before
resuming SPU events. The [whole-machine loader](../state/load-recovery.md)
recovers the preceding state if loading fails.

The component regressions cover stereo register order for all 24 voices,
fixed-gain delay, signed and exponential fades, endpoints, idle voices,
frozen and very slow rates, pending gains and active state restoration.
Generated guest programs check current gains, completed fades, dynamic MMIO
and RAM paths, and strict restored continuation in interpreter, cached and
native slow/LUT/mapped execution with linking enabled and disabled.

```sh
make test TEST_FILTER=SpuSweep
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_spu_volume_live.py
```

Physical sweep-control write phase, manually written voice-current ports and
complete envelope/audio accuracy remain unqualified. No new console capture
or audible-output qualification is claimed.
