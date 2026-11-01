"""Colored CPU block order, DMA placement and restored partial transfers."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from . import spatial_program as guest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('mdec_spatial_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class MdecSpatialLiveTest(unittest.TestCase):
    def test_cpu_dma_order_and_restored_progress(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'spatial-\u732b.bin', root / 'spatial-\u03bb.exe'
            write_inputs(bios, exe, guest.build())
            for old in (False, True):
                for mode, memory, linking in configs:
                    with self.subTest(old=old, mode=mode, memory=memory, linking=linking):
                        output = root / f'{old}-{mode}-{memory}-{linking}'
                        result = compare.run_comparison(binary, bios, exe, output, kind='state', modes=(mode, mode),
                            interval=2, checkpoints=2, min_blocks=10, fastmem=memory, linking=linking,
                            restore_at=1, timeout=30, mdec_old_routines=old)
                        self.assertTrue(result['passed'], result)
                        for side, run in zip(('left', 'right'), result['runs']):
                            if mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                            if mode == 'recomp':
                                self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                                if memory == 'lut':
                                    self.assertGreater(run['lut']['loads'], 0); self.assertGreater(run['lut']['stores'], 0)
                            for frame, _ in run['checkpoints']:
                                self.check_result(output / side / f'checkpoint-{frame:08d}.sav', frame, old)

    def check_result(self, path, frame, old):
        compare.validate_checkpoint(path)
        data = path.read_bytes(); self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 94))
        start = struct.unpack_from('<I', data, 212)[0]; payload = data[start:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
        self.assertEqual(struct.unpack_from('<3I', payload, ram + guest.RESULT),
                         (guest.MAGIC, 1 if frame == 2 else 2, len(guest.CASES)))
        marker = b'\x04\0\0\0MDEC'; self.assertEqual(payload.count(marker), 1)
        mdec = payload.index(marker) + len(marker)
        if frame == 2:
            self.assertEqual(struct.unpack_from('<I', payload, mdec + 1050)[0], 124)
            self.assertEqual(payload[mdec + 3886:mdec + 3888], bytes([4, 4]))
        else:
            for index, case in enumerate(guest.CASES):
                expected = guest.expected(case, old)
                offset = ram + guest.PIXELS + index * 0x400
                self.assertEqual(payload[offset:offset + len(expected)], expected, (index, case))


if __name__ == '__main__': unittest.main()
