# Version-92 queued output records

These hexadecimal files contain only MDEC state from original generated
four-block grayscale input. They contain no BIOS, game, CPU or RAM data.
Each record has 3,886 bytes. The full records retain all output words; the
partial records have consumed five words through the CPU port.

They were generated with the source at commit
`f70521f35ccfcbdbe4260a7ff16ff916aca41f6d`. The receipt records the local
producer binary and unencoded record hashes. Reproduce them with a binary
built from that revision:

```sh
python3 scripts/tests/generate_mdec_legacy.py --binary /path/to/old/cupid-ps1 --output build/legacy-mdec
```

The output directory must be new. The generator uses its own firmware and
checks the state version and output FIFO positions before extracting the
MDEC record. Imported records preserve their saved raster stream until it
drains; subsequently decoded colored output uses tiled device order.
