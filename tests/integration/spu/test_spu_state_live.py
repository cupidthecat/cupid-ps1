"""Restore active SPU voices, gain sweeps, capture RAM and reverb histories."""
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from . import state_program as guest


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class SpuStateLiveTest(unittest.TestCase):
    def test_active_audio_state_after_reset_before_restore(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for pal in (False, True):
                bios, exe = root / f'spu-state-{pal}.bin', root / f'spu-state-{pal}.exe'
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

        def record(name):
            marker = struct.pack('<I', len(name)) + name
            self.assertEqual(payload.count(marker), 1)
            return payload.index(marker) + len(marker)

        ram = record(b'Bus') + 64
        magic, region, progress = struct.unpack_from('<3I', payload, ram + guest.RESULT)
        self.assertEqual((magic, region), (guest.MAGIC, int(pal)))
        self.assertGreater(progress, 0x13579BDF + 100)
        start, end = record(b'SPU'), record(b'MDEC') - 8
        spu = payload[start:end]
        self.assertEqual(len(spu), 512 * 1024 + 4461)
        self.assertEqual(struct.unpack_from('<H', spu, 4)[0], 0xFF80)
        self.assertLess(struct.unpack_from('<I', spu)[0], 768)
        self.assertEqual(struct.unpack_from('<I', spu, 10)[0], 0x1010)
        self.assertLess(struct.unpack_from('<I', spu, 942)[0], 64)
        self.assertTrue(any(spu[174:942]), 'reverb histories must contain live samples')
        for voice in (1, 3):
            self.assertEqual(spu[946 + voice * 143 + 138], 3, 'voice must reach sustain')
            self.assertLess((struct.unpack_from('<I', spu, 946 + voice * 143 + 18)[0] >> 12) & 31, 28)
        sound_ram = spu[-512 * 1024:]
        self.assertEqual(sound_ram[0x1000:0x1010], guest.BLOCK)
        captures = [struct.unpack_from('<512h', sound_ram, offset) for offset in (0x800, 0xC00)]
        self.assertTrue(all(any(samples) for samples in captures))
        self.assertNotEqual(*captures)
        self.assertTrue(any(sound_ram[0x8000:]), 'reverb must update its work area')


if __name__ == '__main__': unittest.main()
