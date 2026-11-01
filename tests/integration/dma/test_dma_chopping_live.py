"""Chopped DMA progress and exact saved continuation across CPU paths."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import chopping_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class DmaChoppingLiveTest(unittest.TestCase):
    def test_pending_bursts_survive_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'chop.bin', root / 'chop.exe'
            write_inputs(bios, exe, guest.build())
            for mode, memory, linking in configs:
                with self.subTest(mode=mode, memory=memory, linking=linking):
                    output = root / f'{mode}-{memory}-{linking}'
                    result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                        interval=6, checkpoints=3, min_blocks=10, fastmem=memory, linking=linking,
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
        self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 95))
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 64
        values = struct.unpack_from('<9I', payload, ram + guest.RESULT)
        magic, stage, first_irq, initial_address, initial_count, address, count, dicr, final_irq = values
        self.assertEqual(magic, guest.MAGIC)
        self.assertTrue(first_irq & (1 << 24))
        self.assertEqual((initial_address, initial_count), (guest.SOURCE + 4, 65535))
        self.assertEqual(stage, 1 if frame < 18 else 2)
        if stage == 2:
            self.assertEqual((address, count, dicr), (guest.SOURCE + 65536 * 4, 0, 0x84840000))
            self.assertTrue(final_irq & (1 << 24))


if __name__ == '__main__': unittest.main()
