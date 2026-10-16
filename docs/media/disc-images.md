# Disc images and changing discs

## Disc image formats

`cd_image_open()` detects formats by extension. Implementations live in
`src/util/disc/cd_image*.c`.

| Format | Extensions | Notes |
|--------|-----------|-------|
| CUE/BIN | `.cue` (+ data files referenced by the cuesheet) | MODE1/MODE2/audio tracks. Optional raw subcode. |
| Raw binary | `.bin`, `.img`, `.iso` | Treated as MODE2 raw with a synthesized 2-second pregap. |
| ECM | `.ecm` | Error-Correction-stripped raw image; expanded on-the-fly via `src/util/disc/cd_image_ecm.c`. |
| CHD | `.chd` | MAME compressed format. Parent-CHD chains resolved by SHA-1 (up to 32 levels). Embedded subq. Precacheable. |
| MDS / MDF | `.mds` (+ `.mdf`) | Alcohol 120% format. |
| CCD | `.ccd` (+ `.img` + `.sub`) | CloneCD format with 96-byte interleaved subchannel. |
| PBP | `.pbp` | Decrypted PSP `EBOOT.PBP`. Multi-disc PBP exposes sub-images. |
| M3U playlist | `.m3u` | UTF-8 list of disc images, one per line. Multi-disc swap via F3/F4. |
| Block device | `/dev/sr0`, `/dev/cdrom`, ... | Linux SG_IO + CDROM ioctls. |
| PPF overlay | `<image>.ppf` (sidecar) | PPF v1/v2/v3 patch overlay. Applied automatically when found alongside the disc image. |
| SBI / LSD | `<image>.sbi` / `<image>.lsd` (sidecar) | Subchannel-Q replacement table for LibCrypt-protected discs. |

Subchannel modes: none, raw interleaved, raw uninterleaved.
Track modes: Audio (2352), Mode1 (2048), Mode1 Raw (2352), Mode2 (2336), Mode2 Form 1 (2048), Mode2 Form 2 (2324), Mode2 Form Mix (2332), Mode2 Raw (2352).

`--cdrom-precache` decompresses the whole image into RAM at boot. CHD and CHD-backed M3U entries support this; raw/cue images skip it.

## Multidisc

`.m3u` files contain one disc image per line, paths relative to the playlist. Implementation: `src/util/disc/cd_image_m3u.c`.

| Action | Key |
|--------|-----|
| Previous disc | `F3` |
| Next disc | `F4` |

Cycles within bounds; out-of-range presses are no-ops. The system pauses on swap, the new sub-image is loaded, and execution resumes. Save states preserve the active sub-image index. M3U playlists whose entries are CHD images auto-precache.
