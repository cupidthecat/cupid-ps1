"""GTE arithmetic across CPU paths with complete restored-state continuation."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from . import color_program as guest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('gte_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class GteColorLiveTest(unittest.TestCase):
    def test_color_arithmetic_and_restored_continuation(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'gte-\u732b.bin', root / 'color-\u03bb.exe'
            write_inputs(bios, exe, guest.build())
            for mode, memory, linking in configs:
                with self.subTest(mode=mode, memory=memory, linking=linking):
                    output = root / f'{mode}-{memory}-{linking}'
                    result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                        interval=2, checkpoints=2, min_blocks=10, fastmem=memory, linking=linking,
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
        data = path.read_bytes(); self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 97))
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
        magic, stage, count, completed = struct.unpack_from('<4I', payload, ram + guest.RESULT)
        self.assertEqual((magic, count), (guest.MAGIC, len(guest.CASES)))
        self.assertIn(stage, (1, 2)); self.assertLessEqual(completed, count)
        if frame == 4: self.assertEqual((stage, completed), (2, count))
        for index, case in enumerate(guest.CASES[:completed]):
            captured = struct.unpack_from('<10I', payload, ram + guest.CAPTURES + index * guest.STRIDE)
            kind, value, sf, lm = case
            if kind == 'boundary':
                self.assertEqual(captured, guest.boundary_expected(case), case)
            elif kind == 'color':
                triple = value in (0x16, 0x20, 0x2A, 0x3F)
                zero_mac = lm and value in (0x1B, 0x1C, 0x3F)
                self.assertEqual(captured[:3], (0xA5000000,) * 3 if triple else (*guest.FIFO[1:], 0xA5000000), case)
                for mac in captured[3:6]:
                    if zero_mac: self.assertEqual(mac, 0, case)
                    else: self.assertTrue(mac & 0x80000000, case)
                self.assertEqual(captured[9] & 0x00380000, 0 if zero_mac else 0x00380000, case)
            else:
                self.assertEqual(captured[:3], guest.FIFO, case)
                self.assertEqual(captured[3:6], tuple((v * (1 if sf else 4096)) & 0xFFFFFFFF for v in (-3, -5, -9)), case)


if __name__ == '__main__': unittest.main()
