"""Exercise the real comparison process with generated firmware and executable inputs."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("compare_live", Path(__file__).resolve().parents[1] / "scripts/compare_execution.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get("CUPID_TEST_BINARY"), "set CUPID_TEST_BINARY to run generated-input boots")
class LiveExecutionComparisonTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.bios = self.root / "generated.bin"
        self.media = self.root / "generated.exe"
        # Store/load RAM, consume the load delay, then stay in a stable branch loop.
        words = (0x3C08A003, 0x24091234, 0xAD090000, 0x8D0A0000, 0,
                 0x254B0001, 0x1000FFFF, 0)
        firmware = bytearray(512 * 1024)
        struct.pack_into('<8I', firmware, 0, *words)
        self.bios.write_bytes(firmware)
        executable = bytearray(2048 + 32)
        executable[:8] = b'PS-X EXE'
        struct.pack_into('<4I', executable, 16, 0x80010000, 0, 0x80010000, 32)
        struct.pack_into('<8I', executable, 2048, *words)
        self.media.write_bytes(executable)
        self.binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)

    def run_pair(self, modes, kind, fastmem='off'):
        result = compare.run_comparison(self.binary, self.bios, self.media, self.root / 'run',
                                        kind=kind, modes=modes, instructions=1000,
                                        interval=2, checkpoints=2, min_blocks=10,
                                        fastmem=fastmem, timeout=30)
        self.assertTrue(result['passed'], result)

    def test_generated_instruction_trace(self):
        self.run_pair(('interp', 'cached'), 'trace')

    def test_generated_machine_checkpoints(self):
        self.run_pair(('interp', 'cached'), 'state')

    def test_native_linked_checkpoints(self):
        self.run_pair(('recomp', 'recomp'), 'state')

    def test_unimplemented_lut_path_cannot_qualify_as_fastmem(self):
        # The native backend currently supports mmap only. Keep this as a
        # failure until LUT execution is implemented, rather than accepting fallback.
        result = compare.run_comparison(self.binary, self.bios, self.media, self.root / 'run',
                                        kind='state', modes=('recomp', 'recomp'), fastmem='lut',
                                        interval=2, checkpoints=2, min_blocks=10, timeout=30)
        self.assertFalse(result['passed'], result)
        self.assertTrue(all('unexpected execution configuration' in ' '.join(run['failures'])
                            for run in result['runs']), result)

    def test_native_linked_mmap_checkpoints(self):
        self.run_pair(('recomp', 'recomp'), 'state', 'mmap')

    def test_native_cross_engine_checkpoint_difference_is_reported(self):
        for fastmem in ('off', 'mmap'):
            with self.subTest(fastmem=fastmem):
                result = compare.run_comparison(self.binary, self.bios, self.media,
                                                self.root / ('run-' + fastmem),
                                                kind='state', modes=('interp', 'recomp'), fastmem=fastmem,
                                                interval=2, checkpoints=2, min_blocks=10, timeout=30)
                self.assertTrue(all(run['passed'] for run in result['runs']), result)
                self.assertFalse(result['passed'], result)
                self.assertTrue(result['differences'], result)


if __name__ == '__main__':
    unittest.main()
