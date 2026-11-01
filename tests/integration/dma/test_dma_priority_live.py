"""Programmed DMA ordering and exact continuation of armed channels."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import priority_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class DmaPriorityLiveTest(unittest.TestCase):
    def test_priority_pairs_and_armed_channels_survive_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'priority.bin', root / 'priority.exe'
            write_inputs(bios, exe, guest.build())
            for mode, memory, linking in configs:
                with self.subTest(mode=mode, memory=memory, linking=linking):
                    output = root / f'{mode}-{memory}-{linking}'
                    result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                        interval=3, checkpoints=2, min_blocks=10, fastmem=memory, linking=linking,
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
        magic, stage, count, completed = struct.unpack_from('<4I', payload, ram + guest.RESULT)
        self.assertEqual((magic, count), (guest.MAGIC, len(guest.CASES)))
        self.assertEqual((stage, completed), (1, 0) if frame == 3 else (2, count))
        for index, (read, gpu, otc) in enumerate(guest.CASES[:completed]):
            word, gpu_status, dicr, gpu_control, otc_control = struct.unpack_from(
                '<5I', payload, ram + guest.ROWS + index * 20)
            expected = (0x00FFFFFF if gpu < otc else guest.PIXEL) if read else 0x00FFFFFF
            self.assertEqual(word, expected, (read, gpu, otc))
            self.assertEqual(bool(gpu_status & (1 << 24)), not read and gpu < otc, (read, gpu, otc))
            self.assertEqual(dicr, 0xC4C40000)
            self.assertFalse(gpu_control & (1 << 24)); self.assertEqual(otc_control, 2)


if __name__ == '__main__': unittest.main()
