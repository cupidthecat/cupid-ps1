"""Check guest-program noise rates, shared capture inputs and restored phases."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from ..guest_program import write_inputs
from . import noise_program

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('noise_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class SpuNoiseLiveTest(unittest.TestCase):
    def test_rates_capture_and_restored_continuation(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bios, executable = directory / 'noise-\u732b.bin', directory / 'noise-\u03bb.exe'
            write_inputs(bios, executable, noise_program.build())
            configs = [('interp', 'off', True), ('cached', 'off', True)]
            configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
            for mode, fastmem, linking in configs:
                with self.subTest(mode=mode, fastmem=fastmem, linking=linking):
                    output = directory / f'{mode}-{fastmem}-{linking}'
                    result = compare.run_comparison(binary, bios, executable, output,
                        kind='state', modes=(mode, mode), interval=18, checkpoints=2, min_blocks=100,
                        fastmem=fastmem, linking=linking, restore_at=1, timeout=45)
                    self.assertTrue(result['passed'], result)
                    for side, run in zip(('left', 'right'), result['runs']):
                        if mode == 'recomp':
                            self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                            if fastmem == 'lut':
                                self.assertGreater(run['lut']['loads'], 0); self.assertGreater(run['lut']['stores'], 0)
                        elif mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                        for frame, ticks in run['checkpoints']:
                            self.check_noise(output / side / f'checkpoint-{frame:08d}.sav', ticks)

    def check_noise(self, path, ticks):
        data = path.read_bytes()
        self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 96))
        compression, size, unpacked, offset = struct.unpack_from('<4I', data, 200)
        self.assertEqual(compression, 0); self.assertEqual(size, unpacked)
        self.assertEqual(offset + size, len(data))
        payload = data[offset:]
        marker = b'\x03\0\0\0SPU'; self.assertEqual(payload.count(marker), 1)
        start = payload.index(marker) + len(marker)
        end_marker = b'\x04\0\0\0MDEC'; self.assertEqual(payload.count(end_marker), 1)
        end = payload.index(end_marker, start)
        self.assertEqual(struct.unpack_from('<H', payload, start + 4)[0], 0xC000 | noise_program.CLOCK << 8)
        samples = ticks // 768
        # Clock17 has a1024-sample divider period and adds5 to the 3-bit counter.
        carries = samples // 1024
        shifts = carries * 5 // 8
        expected = 1
        for _ in range(shifts):
            feedback = ((expected >> 15) ^ (expected >> 12) ^ (expected >> 11) ^ (expected >> 10) ^ 1) & 1
            expected = ((expected << 1) | feedback) & 0xFFFFFFFF
        self.assertEqual(struct.unpack_from('<I', payload, start + 88)[0], expected)
        phase = end - 512 * 1024 - 83
        self.assertEqual(struct.unpack_from('<HB', payload, phase), ((samples % 1024) * 32, carries * 5 & 7))
        self.assertNotEqual(payload[phase:phase + 3], bytes(3))
        self.assertEqual(struct.unpack_from('<H', payload, start + 18)[0], samples * 2 % 1024)
        ram = payload[end - 512 * 1024:end]
        voice1 = struct.unpack_from('<512h', ram, 0x800)
        voice3 = struct.unpack_from('<512h', ram, 0xC00)
        self.assertEqual(voice1, voice3)
        self.assertTrue(any(voice1))


if __name__ == '__main__': unittest.main()
