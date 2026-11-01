# Disc identification

The third byte of the `GetID` completion response identifies the TOC disc
type. Audio and Mode 1 discs use `00h`; Mode 2 discs use `20h`. See the
[CD-ROM command specification](https://psx-spx.consoledev.net/cdromdrive/).

The image layer retains an explicit A0 `PSec` value from a CCD descriptor.
When no explicit value is available, it synthesizes the type from the track
modes: a Mode 2 track selects `20h`; audio and Mode 1 tracks select `00h`.
TOC copies, playlists and memory-backed copies retain the explicit value.
Clearing the TOC clears that metadata as well.

This field does not change licensing or region checks, other response bytes,
interrupt selection, command latency or the current state layout at version
92. MDS and physical-drive A0 metadata, additional session/ATIP information,
mixed-disc identification flags and exact response timing remain unqualified.

Generated component checks cover track-mode synthesis, explicit CCD values,
TOC copying/clearing, playlists, memory copies, both command responses,
pending-response restoration and metadata retention across reset. The guest
check uses generated audio, Mode 1 and Mode 2 CUE/BIN inputs through CPU MMIO
paths.

```sh
make test TEST_FILTER=CdIdentification
make test TEST_FILTER=CDImageDiscType
CUPID_TEST_BINARY=build/cupid-ps1 python3 -m unittest discover -s tests -p test_cdrom_identification_live.py
```

These checks use generated inputs. They do not qualify commercial-disc
compatibility, physical drives or every image format.
