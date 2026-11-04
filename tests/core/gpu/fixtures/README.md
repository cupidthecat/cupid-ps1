# Version-93 polyline records

The header contains four compressed GPU-only records written by revision
`f391d012b8736c91f8dac5a7e766a83decae1d87`. Flat and shaded packets were saved
while pending at frame 3 and after completion at frame 6. These records contain
no BIOS, RAM, game data or save-container metadata. The JSON receipt records
their hashes, the original producer binary hash and the exact synthetic inputs.

Build that revision and reproduce the records in a new output directory:

```sh
python3 scripts/tests/generate_gpu_polyline_legacy.py \
  --binary /path/to/old/cupid-ps1 --output build/legacy-gpu-polylines
```

The generator checks the actual producer version, synthetic inputs and every
uncompressed record against the receipt. It does not modify version headers.
Version 94 can load the idle records. Pending version-93 records lack the
vertices needed to resume and are rejected explicitly.
