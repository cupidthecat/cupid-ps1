"""GPU gate edges must preserve another counter's pending interrupt."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import gate_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class TimerGatesLiveTest(unittest.TestCase):
    def test_shared_irq_and_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for gated, pal in ((gated, pal) for gated in (False, True) for pal in (False, True)):
                bios, exe = root / f'gates-{gated}-{pal}.bin', root / f'gates-{gated}-{pal}.exe'
                write_inputs(bios, exe, guest.build(gated, pal))
                for mode, memory, linking in configs:
                    with self.subTest(gated=gated, pal=pal, mode=mode, memory=memory, linking=linking):
                        output = root / f'{gated}-{pal}-{mode}-{memory}-{linking}'
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
                                self.check_result(output / side / f'checkpoint-{frame:08d}.sav', frame, gated, pal)

    def check_result(self, path, frame, gated, pal):
        compare.validate_checkpoint(path)
        data = path.read_bytes()
        payload = data[struct.unpack_from('<I', data, 212)[0]:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        ram = payload.index(marker) + len(marker) + 64
        magic, stage, selected, status, copied = struct.unpack_from('<5I', payload, ram + guest.RESULT)
        self.assertEqual((magic, selected), (guest.MAGIC, int(gated) | int(pal) << 1))
        if frame == 3:
            self.assertEqual((stage, status, copied), (2, 0, 0))
            def record(name):
                marker = struct.pack('<I', len(name)) + name
                self.assertEqual(payload.count(marker), 1)
                return payload.index(marker) + len(marker)

            irq = struct.unpack_from('<I', payload, record(b'InterruptController'))[0]
            self.assertFalse(irq & guest.TIMER_IRQ)
            timer = record(b'Timers') + 2 * 18
            mode, counter, target = struct.unpack_from('<3I', payload, timer)
            self.assertEqual(mode & 0xA10, 0x210)
            self.assertLess(counter, 65535)
            self.assertEqual(target, 65535)
            self.assertEqual(payload[timer + 16:timer + 18], b'\0\0')
            events = record(b'Events')
            ticks, _, count = struct.unpack_from('<2QI', payload, events)
            cursor, shared = events + 20, []
            for _ in range(count):
                length = struct.unpack_from('<I', payload, cursor)[0]
                cursor += 4
                name = payload[cursor:cursor + length]; cursor += length
                deadline, _ = struct.unpack_from('<2Q', payload, cursor)
                if name == b'Timer SysClk Interrupt': shared.append(deadline)
                cursor += 24
            self.assertEqual(len(shared), 1)
            self.assertGreater(shared[0], ticks)
        else:
            self.assertEqual((stage, copied), (3, guest.MAGIC))
            self.assertTrue(status & guest.TIMER_IRQ, f'missing Timer 2 interrupt: {status:08x}')


if __name__ == '__main__': unittest.main()
