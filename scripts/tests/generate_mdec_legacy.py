#!/usr/bin/env python3
"""Generate version-92 MDEC records with an explicitly supplied older binary."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
from integration.guest_program import Program, write_inputs

spec = importlib.util.spec_from_file_location('legacy_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


def build(depth, consumed):
    program = Program()
    program.emit(0x40806000, 0, 0, 0)
    program.constant(16, 0x1F801820)
    program.constant(8, 0x80000000); program.emit(0xAE080004)

    def write(value):
        program.constant(8, value); program.emit(0xAE080000, *([0] * 16))

    write(0x40000001)
    for _ in range(32): write(0)
    write(0x60000000)
    for _ in range(32): write(0x40004000)
    write(0x20000006 | depth << 27)
    for dc in (0, 0, -128, -64, 64, 128): write(0xFE000000 | (dc & 0x3FF))
    program.constant(9, 0x80000000)
    for word in range(consumed):
        program.label(f'read-{word}')
        program.emit(0x8E080004, 0, 0x01094024)
        program.branch(5, 8, 0, f'read-{word}')
        program.emit(0x8E0A0000, 0)
    program.label('halt'); program.branch(4, 0, 0, 'halt')
    return program.finish()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=False)
    binary_hash = compare.sha256(binary)
    records = []
    for depth, bits in ((3, 15), (2, 24)):
        for consumed in (0, 5):
            label = f'color{bits}-{"partial" if consumed else "full"}-v92'
            folder = output / label; folder.mkdir()
            shutil.copytree(ROOT / 'data', folder / 'data')
            bios, media = folder / 'generated.bin', folder / 'generated.exe'
            write_inputs(bios, media, build(depth, consumed))
            env = {key: value for key, value in os.environ.items() if not key.startswith('CUPID_')}
            env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CUPID_GPU_SW_ISA='SCALAR',
                       CUPID_CHECKPOINT_DIR=str(folder), CUPID_CHECKPOINT_SCHEDULE='2,1')
            result = compare.wait_child([str(binary), '--cpu-mode', 'interp', '--fastmem', 'off',
                '--gpu-renderer', 'software', '--pgxp', 'off', '--bios', str(bios), str(media)], folder, env, 30)
            assert result['exit_code'] == 0 and not result['timed_out'], result
            state = (folder / 'checkpoint-00000002.sav').read_bytes()
            assert struct.unpack_from('<2I', state) == (0x43435544, 92), 'requires a version-92 binary'
            marker = b'\x04\0\0\0MDEC'; assert state.count(marker) == 1
            start = state.index(marker) + len(marker)
            record = state[start:start + 3886]
            count = 128 if depth == 3 else 192
            assert struct.unpack_from('<3I', record, 1042) == (consumed, count % 192, count - consumed)
            assert record[-1] == 0
            path = output / (label + '.hex')
            path.write_text('\n'.join(record[i:i + 32].hex() for i in range(0, len(record), 32)) + '\n')
            records.append(dict(file=path.name, bytes=len(record), sha256=hashlib.sha256(record).hexdigest(),
                                depth=depth, consumed=consumed))
    assert compare.sha256(binary) == binary_hash
    (output / 'receipt.json').write_text(json.dumps(dict(binary_sha256=binary_hash, records=records,
        scope='Generated firmware and compressed-data constants only; no BIOS or game input.'), indent=2))
    print('Generated four version-92 MDEC records.')


if __name__ == '__main__': main()
