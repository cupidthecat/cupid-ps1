"""Physical DMA limits and exact continuation across CPU and memory paths."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import bus_error_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class DmaBusErrorsLiveTest(unittest.TestCase):
    def test_faulted_prefix_and_gpu_fifo_survive_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); bios, exe = root / 'dma.bin', root / 'dma.exe'
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
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 64
        magic, stage, count, completed = struct.unpack_from('<4I', payload, ram + guest.RESULT)
        self.assertEqual((magic, count), (guest.MAGIC, len(guest.CASES)))
        self.assertEqual((stage, completed), (1, 0) if frame == 3 else (2, count))
        for index in range(max(completed, 1)):
            name, channel, address, block, control, read = guest.CASES[index]
            fault = name not in ('otc-last', 'last-header')
            dicr = guest.ENABLES | (0x80008000 if fault else 0x80000000 | 1 << (24 + channel))
            base = 0xFFFFFF if name.startswith('linked-') or name == 'last-header' else address
            bcr = 0x10002 if name.startswith('request-') else block
            ram0 = ram4 = last = guest.SENTINEL
            if name in ('otc-zero', 'otc-four'): ram0 = 0x1FFFFC
            if name == 'otc-four': ram4 = 0
            if name == 'otc-last': last = 0xFFFFFF
            if read: last = 0x12345678
            if name in ('write-forward', 'request-write', 'linked-prefix'): last = 0x1F000000
            if name == 'write-reverse': ram0 = 0x1F000000
            if name == 'last-header': last = 0xFFFFFF
            if name == 'linked-no-payload': last = 0x01FFFFFF
            gpu_irq = (1 << 24) if name in ('write-forward', 'write-reverse', 'request-write', 'linked-prefix') else 0
            chcr = 2 if channel == 6 else control & ~0x11000000
            expected = (chcr, base, bcr, dicr, ram0, ram4, last, gpu_irq, 8)
            actual = struct.unpack_from('<10I', payload, ram + guest.RESULT + 16 + index * 40)
            self.assertEqual(actual[:9], expected, name)
            if completed: self.assertEqual(actual[9], 0x23456789 if read else 0, name)


if __name__ == '__main__': unittest.main()
