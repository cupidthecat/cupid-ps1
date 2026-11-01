"""Partial-store device effects and complete restored-state continuation."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from . import partial_program as guest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('partial_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class PartialStoresLiveTest(unittest.TestCase):
    def test_devices_and_pending_response_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'partial-\u732b.bin', root / 'partial-\u03bb.exe'
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
        data = path.read_bytes(); self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 96))
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
        self.assertEqual(struct.unpack_from('<3I', payload, ram + guest.RESULT),
                         (guest.MAGIC, 1 if frame == 2 else 2, len(guest.CASES)))
        self.assertTrue(struct.unpack_from('<I', payload, ram + guest.RESULT + 0xC)[0] & 0x20)
        if frame == 2:
            self.assertEqual(struct.unpack_from('<I', payload, ram + guest.RESULT + 0x10)[0], 0xFFFFFFFF)
        else:
            for offset in (0x10, 0x18):
                self.assertTrue(struct.unpack_from('<I', payload, ram + guest.RESULT + offset)[0] & 0x20)
            self.assertEqual(struct.unpack_from('<2I', payload, ram + guest.RESULT + 0x30), (3, 3))
            self.assertEqual(payload[ram + guest.RESULT + 0x42:ram + guest.RESULT + 0x45], b'\0\x12\x34')
            self.assertFalse(struct.unpack_from('<I', payload, ram + guest.RESULT + 0x48)[0] & 0x20)
            for i, case in enumerate(guest.CASES):
                self.assertEqual(struct.unpack_from('<2I', payload, ram + guest.CAPTURES + i * 8),
                                 guest.expected(case), case)


if __name__ == '__main__': unittest.main()
