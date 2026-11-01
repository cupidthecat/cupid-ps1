"""Information-query mirrors, stable read latch and reset-before-restore."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from .test_gpu_rectangle_live import compare
from . import information_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class GpuInformationLiveTest(unittest.TestCase):
    def test_information_queries_and_restored_latch(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'information.bin', root / 'information.exe'
            write_inputs(bios, exe, guest.build())
            for mode, memory, linking in configs:
                with self.subTest(mode=mode, memory=memory, linking=linking):
                    output = root / f'{mode}-{memory}-{linking}'
                    result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                        interval=3, checkpoints=3, min_blocks=10, fastmem=memory, linking=linking,
                        restore_at=1, restore_reset=True, timeout=30)
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
        data = path.read_bytes()
        self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 96))
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 64
        magic, stage, count, completed, restored, unknown, before, after, copied = struct.unpack_from(
            '<9I', payload, ram + guest.RESULT)
        self.assertEqual((magic, count), (guest.MAGIC, len(guest.CASES)))
        self.assertIn(stage, (1, 2)); self.assertLessEqual(completed, count)
        self.assertFalse(before & ((1 << 25) | (1 << 27)))
        if frame == 3:
            self.assertEqual((stage, completed, restored, unknown, copied), (1, 0, 0, 0, 0))
        else:
            self.assertEqual((restored, unknown, copied), (2, 2, guest.MAGIC))
            self.assertFalse(after & ((1 << 25) | (1 << 27)))
        if frame == 9: self.assertEqual((stage, completed), (2, count))
        for number, (command, index) in enumerate(guest.CASES[:completed]):
            actual = struct.unpack_from('<5I', payload, ram + guest.ROWS + number * 20)
            value = guest.expected(index)
            self.assertEqual(actual, (command << 24 | 0x765400 | index, guest.SEED, value, value, value),
                             (command, index))


if __name__ == '__main__': unittest.main()
