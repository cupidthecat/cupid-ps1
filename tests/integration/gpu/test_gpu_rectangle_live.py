"""Texture orientation and complete state continuation through guest MMIO."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from . import rectangle_program as guest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('gpu_rectangle_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class GpuRectangleLiveTest(unittest.TestCase):
    def test_rectangle_pixels_and_restored_packet(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'gpu-\u732b.bin', root / 'rect-\u03bb.exe'
            write_inputs(bios, exe, guest.build())
            for mode, memory, linking in configs:
                with self.subTest(mode=mode, memory=memory, linking=linking):
                    output = root / f'{mode}-{memory}-{linking}'
                    result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                        interval=3, checkpoints=2, min_blocks=10, fastmem=memory, linking=linking,
                        restore_at=1, timeout=30)
                    self.assertTrue(result['passed'], result)
                    for side, run in zip(('left', 'right'), result['runs']):
                        if mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                        if mode == 'recomp':
                            self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                            if memory == 'lut':
                                self.assertGreater(run['lut']['loads'], 0); self.assertGreater(run['lut']['stores'], 0)
                        for frame, _ in run['checkpoints']:
                            self.check_result(output / side / f'checkpoint-{frame:08d}.sav', frame)

    def check_result(self, path, frame):
        compare.validate_checkpoint(path)
        data = path.read_bytes(); self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 93))
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
        magic, stage, count, completed = struct.unpack_from('<4I', payload, ram + guest.RESULT)
        self.assertEqual((magic, count), (guest.MAGIC, len(guest.CASES)))
        self.assertIn(stage, (1, 2)); self.assertLessEqual(completed, count)
        if frame == 3: self.assertEqual((stage, completed), (1, 0))
        if frame == 6: self.assertEqual((stage, completed), (2, count))
        for index, case in enumerate(guest.CASES[:completed]):
            actual = struct.unpack_from('<32H', payload, ram + guest.PIXELS + index * 64)
            self.assertEqual(actual, guest.expected(case), case)


if __name__ == '__main__': unittest.main()
