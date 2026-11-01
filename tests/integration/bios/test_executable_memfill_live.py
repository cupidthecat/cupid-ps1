"""Generated executable captures overlapping initialization before running its loop."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import Program

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('exe_fill_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)
COMPLETE = 0x46494C4C


def write_inputs(bios, executable, overlap):
    boot = Program()
    boot.constant(29, 0x801F9000); boot.constant(30, 0xA0A10000); boot.constant(28, 0xABCD0000)
    boot.constant(8, 0x1F802041); boot.constant(9, 7)
    boot.emit(0xA1090000)  # POST 07 invokes the executable loader.
    boot.label('failed'); boot.branch(4, 0, 0, 'failed')
    words = boot.finish()
    firmware = bytearray(512 * 1024)
    struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
    bios.write_bytes(firmware)

    entry = Program()
    entry.constant(8, 0xA0030000); entry.constant(9, 0)
    data_pointer = len(entry.words) - 2
    for offset in (0, 4, 8): entry.emit(0x8D2A0000 | offset, 0, 0xAD0A0000 | offset)
    entry.constant(10, COMPLETE); entry.emit(0xAD0A000C)
    entry.label('loop'); entry.emit(0x8D0A000C, 0, 0xAD0A0010)
    entry.branch(4, 0, 0, 'loop')
    words = entry.finish()
    address = 0x80010000 + len(words) * 4
    words[data_pointer] |= address >> 16
    words[data_pointer + 1] |= address & 0xFFFF
    words.extend((0x11223344, 0x55667788, 0x99AABBCC))
    image = bytearray(2048 + len(words) * 4)
    image[:8] = b'PS-X EXE'
    struct.pack_into('<4I', image, 16, 0x80010000, 0, 0x80010000, len(words) * 4)
    struct.pack_into('<2I', image, 0x28, address if overlap else address + 0x1000, 8)
    struct.pack_into('<I', image, 0x30, 0x801FC000)
    struct.pack_into('<' + 'I' * len(words), image, 2048, *words)
    executable.write_bytes(image)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class ExecutableMemfillLiveTest(unittest.TestCase):
    def test_initialized_payload_and_restored_continuation(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bios, media = directory / 'boot-\u732b.bin', directory / 'fill-\u03bb.exe'
            for overlap in (False, True):
                write_inputs(bios, media, overlap)
                expected = (0 if overlap else 0x11223344, 0 if overlap else 0x55667788,
                            0x99AABBCC, COMPLETE, COMPLETE)
                for mode, fastmem, linking in configs:
                    with self.subTest(overlap=overlap, mode=mode, fastmem=fastmem, linking=linking):
                        output = directory / f'{overlap}-{mode}-{fastmem}-{linking}'
                        result = compare.run_comparison(binary, bios, media, output,
                            kind='state', modes=(mode, mode), interval=2, checkpoints=2, min_blocks=10,
                            fastmem=fastmem, linking=linking, restore_at=1, timeout=30)
                        self.assertTrue(result['passed'], result)
                        for side, run in zip(('left', 'right'), result['runs']):
                            self.assertIn('Injected EXE:', (output / side / 'boot.log').read_text(errors='replace'))
                            if mode == 'recomp':
                                self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                                if fastmem == 'lut':
                                    self.assertGreater(run['lut']['loads'], 0)
                                    self.assertGreater(run['lut']['stores'], 0)
                            elif mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                            for frame, _ in run['checkpoints']:
                                path = output / side / f'checkpoint-{frame:08d}.sav'
                                compare.validate_checkpoint(path)
                                data = path.read_bytes()
                                self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 93))
                                start = struct.unpack_from('<I', data, 212)[0]
                                payload = data[start:]
                                marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
                                ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
                                self.assertEqual(struct.unpack_from('<5I', payload, ram + 0x30000), expected)


if __name__ == '__main__': unittest.main()
