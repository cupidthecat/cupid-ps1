"""Run MDEC command boundaries through actual guest CPU memory paths."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest
from ..guest_program import write_inputs
from . import command_program

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('mdec_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class MdecCommandsLiveTest(unittest.TestCase):
    def test_no_function_status_command_boundaries_and_output(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bios, executable = directory / 'mdec-\u732b.bin', directory / 'mdec-\u03bb.exe'
            write_inputs(bios, executable, command_program.build())
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
                        for frame, _ in run['checkpoints']:
                            self.check_result(output / side / f'checkpoint-{frame:08d}.sav')

    def check_result(self, path):
        data = path.read_bytes()
        self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 92))
        compression, size, unpacked, offset = struct.unpack_from('<4I', data, 200)
        self.assertEqual(compression, 0); self.assertEqual(size, unpacked)
        self.assertEqual(offset + size, len(data))
        payload = data[offset:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        start = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4 + command_program.RESULT_ADDRESS
        self.assertEqual(struct.unpack_from('<5I', payload, start),
                         (command_program.RESULT_MAGIC, 0x9584FFFF, 0x80808080, 0x80808080, 30))
        self.assertEqual(struct.unpack_from('<I', payload, start + 24)[0], 0x9584FFFF)


if __name__ == '__main__': unittest.main()
