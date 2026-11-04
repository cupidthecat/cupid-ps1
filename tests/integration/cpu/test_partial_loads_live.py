"""CPU load cancellation, selected peripheral reads and saved continuation."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import partial_load_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class PartialLoadsLiveTest(unittest.TestCase):
    def test_load_replacement_and_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for pal in (False, True):
                bios, exe = root / f'loads-{pal}.bin', root / f'loads-{pal}.exe'
                write_inputs(bios, exe, guest.build(pal))
                for mode, memory, linking in configs:
                    with self.subTest(pal=pal, mode=mode, memory=memory, linking=linking):
                        output = root / f'{pal}-{mode}-{memory}-{linking}'
                        result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                            interval=3, checkpoints=2, min_blocks=10, fastmem=memory, linking=linking,
                            restore_at=1, restore_reset=True, timeout=30)
                        self.assertTrue(result['passed'], result)
                        for side, run in zip(('left', 'right'), result['runs']):
                            if mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                            if mode == 'recomp':
                                self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                                if memory == 'lut':
                                    self.assertGreater(run['lut']['loads'], 0)
                                    self.assertGreater(run['lut']['stores'], 0)
                            for frame, _ in run['checkpoints']:
                                self.check_result(output / side / f'checkpoint-{frame:08d}.sav', pal)

    def check_result(self, path, pal):
        compare.validate_checkpoint(path)
        data = path.read_bytes()
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 64
        magic, loops, region, flags, pending, serial, errors, cache, ram_size = struct.unpack_from('<9I', payload, ram + guest.RESULT)
        self.assertEqual((magic, region), (guest.MAGIC, int(pal)))
        self.assertGreater(loops, 1)
        self.assertEqual(errors, 0, 'sticky load-pair mismatch, including instructions after restoration')
        expected = guest.expected()
        actual = struct.unpack_from(f'<{len(expected)}I', payload, ram + guest.RESULT + 40)
        for i, value in enumerate(expected):
            self.assertEqual(actual[i], value, f'load case {i // 2}, {"delay" if i & 1 else "value"}')
        self.assertEqual((flags, pending, serial), (0x123456E3, 0x20, 0x12341003))
        self.assertEqual((cache, ram_size), (0x12340001, 0x12340000))


if __name__ == '__main__': unittest.main()
