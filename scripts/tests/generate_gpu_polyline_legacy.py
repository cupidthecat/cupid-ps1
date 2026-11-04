#!/usr/bin/env python3
"""Reproduce GPU-only polyline records using an explicitly supplied version-93 binary."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tests.integration.guest_program import write_inputs
from tests.integration.gpu import legacy_polyline_program
from tests.integration.gpu.test_gpu_rectangle_live import compare


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    metadata = json.loads((ROOT / 'tests/core/gpu/fixtures/polyline_v93.json').read_text())
    binary_hash = compare.sha256(binary)
    records = []
    for stimulus in metadata['stimuli']:
        shaded = stimulus['shaded']
        folder = output / ('shaded' if shaded else 'flat'); folder.mkdir()
        bios, media = folder / 'generated.bin', folder / 'generated.exe'
        write_inputs(bios, media, legacy_polyline_program.build(shaded))
        if (compare.sha256(bios), compare.sha256(media)) != (
                stimulus['firmware_sha256'], stimulus['executable_sha256']):
            raise ValueError('generated inputs differ from the recorded stimulus')
        result = compare.run_comparison(binary, bios, media, folder / 'run', kind='state',
            modes=('interp', 'interp'), interval=3, checkpoints=2, min_blocks=1, timeout=30)
        if not result['passed']: raise ValueError(result)
        for frame in (3, 6):
            path = folder / 'run/left' / f'checkpoint-{frame:08d}.sav'
            compare.validate_checkpoint(path)
            state = path.read_bytes()
            if struct.unpack_from('<2I', state) != (0x43435544, 93):
                raise ValueError('requires a version-93 producer; do not patch the version header')
            payload = state[struct.unpack_from('<I', state, 212)[0]:]
            begin, end = b'\x03\0\0\0GPU', b'\x05\0\0\0CDROM'
            if payload.count(begin) != 1 or payload.count(end) != 1:
                raise ValueError('ambiguous GPU record boundaries')
            start = payload.index(begin) + len(begin)
            record = payload[start:payload.index(end, start)]
            name = f'polyline_v93_{"shaded" if shaded else "flat"}_{"pending" if frame == 3 else "idle"}'
            expected = next(r for r in metadata['records'] if r['name'] == name)
            digest = hashlib.sha256(record).hexdigest()
            if len(record) != expected['uncompressed_bytes'] or digest != expected['sha256']:
                raise ValueError(f'{name} differs from the original writer output')
            compressed = zlib.compress(record, 9)
            (output / (name + '.zlib')).write_bytes(compressed)
            records.append(dict(name=name, uncompressed_bytes=len(record), sha256=digest,
                                compressed_sha256=hashlib.sha256(compressed).hexdigest()))
    if compare.sha256(binary) != binary_hash: raise ValueError('producer binary changed')
    (output / 'receipt.json').write_text(json.dumps(dict(binary_sha256=binary_hash, records=records,
        scope='Synthetic firmware/executable only. Extracted records contain GPU data, no BIOS or RAM.'), indent=2))
    print('Reproduced all four original version-93 GPU records.')


if __name__ == '__main__': main()
