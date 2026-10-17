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
        self.write_program(words)
        self.binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)

    def write_program(self, words):
        firmware = bytearray(512 * 1024)
        struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
        self.bios.write_bytes(firmware)
        size = len(words) * 4
        executable = bytearray(2048 + size)
        executable[:8] = b'PS-X EXE'
        struct.pack_into('<4I', executable, 16, 0x80010000, 0, 0x80010000, size)
        struct.pack_into('<' + 'I' * len(words), executable, 2048, *words)
        self.media.write_bytes(executable)

    def run_pair(self, modes, kind, fastmem='off'):
        result = compare.run_comparison(self.binary, self.bios, self.media, self.root / 'run',
                                        kind=kind, modes=modes, instructions=1000,
                                        interval=2, checkpoints=2, min_blocks=10,
                                        fastmem=fastmem, timeout=30)
        self.assertTrue(result['passed'], result)

    def test_generated_instruction_trace(self):
        self.run_pair(('interp', 'cached'), 'trace')

    def test_reserved_cached_word_faults_after_a_ram_rewrite(self):
        words = []

        def constant(reg, value):
            words.extend((0x3C000000 | (reg << 16) | (value >> 16),
                          0x34000000 | (reg << 21) | (reg << 16) | (value & 0xFFFF)))

        # Firmware installs a handler and RAM code, then jumps to its cached alias.
        constant(8, 0xFFFE0130)
        constant(9, 0x1E988)
        words.extend((0xAD090000, 0x40806000))
        constant(15, 0xA0000000)
        handler = (0x40106800, 0x40127000, 0x25530000, 0x1000FFFF, 0)
        for index, instruction in enumerate(handler):
            constant(14, instruction)
            words.append(0xADEE0080 | (index * 4))
        constant(15, 0xA0010020)
        program = (0xAD280008, 0x8D6A0000, 0x74000000, 0x26310001, 0x1000FFFF, 0)
        for index, instruction in enumerate(program):
            constant(14, instruction)
            words.append(0xADEE0000 | (index * 4))
        constant(8, 0x24140063)
        constant(9, 0xA0010020)
        constant(10, 7)
        constant(11, 0xA0030000)
        constant(14, 42)
        words.append(0xAD6E0000)
        constant(24, 0x80010020)
        words.extend((0x03000008, 0))
        self.write_program(words)

        for mode, fastmem, linking in (('interp', 'off', True), ('cached', 'off', True),
                                      ('recomp', 'off', True), ('recomp', 'mmap', True),
                                      ('recomp', 'off', False), ('recomp', 'lut', True), ('recomp', 'lut', False)):
            with self.subTest(mode=mode, fastmem=fastmem, linking=linking):
                directory = self.root / f'reserved-{mode}-{fastmem}-{linking}'
                result = compare.run_comparison(self.binary, self.bios, self.media, directory,
                                                kind='state', modes=(mode, mode), fastmem=fastmem,
                                                linking=linking, interval=2, checkpoints=2,
                                                min_blocks=10, timeout=30)
                self.assertTrue(result['passed'], result)
                if mode == 'recomp':
                    # Timing events can resume the handler's NOP slot through the shared executor.
                    # Every other instruction, including the reserved word, must execute natively.
                    for run in result['runs']:
                        self.assertEqual(run['execution']['interpreted'],
                                         run['execution']['native_dispatch_delay_slots'], result)
                for side in ('left', 'right'):
                    for frame in (2, 4):
                        data = (directory / side / f'checkpoint-{frame:08d}.sav').read_bytes()
                        self.assertEqual(struct.unpack_from('<II', data), (0x43435544, 87))
                        size, unpacked, start = struct.unpack_from('<III', data, 204)
                        self.assertEqual(size, unpacked)
                        self.assertEqual(start + size, len(data))
                        self.assertEqual(data[start:start + 10], b'\x06\0\0\0System')
                        cpu = start + 39
                        self.assertEqual(data[cpu:cpu + 7], b'\x03\0\0\0CPU')
                        # Four cycle fields precede the GPRs in the version 87 CPU record.
                        regs = struct.unpack_from('<32I', data, cpu + 7 + 16)
                        self.assertEqual((regs[16] >> 2) & 31, 10)
                        self.assertEqual(regs[18], 0x80010028)
                        self.assertEqual(regs[19], 42)
                        self.assertEqual(regs[10], 42)
                        self.assertEqual(regs[17], 0)
                        self.assertEqual(regs[20], 0)

    def test_generated_machine_checkpoints(self):
        self.run_pair(('interp', 'cached'), 'state')

    def test_native_linked_checkpoints(self):
        self.run_pair(('recomp', 'recomp'), 'state')

    def test_native_lut_checkpoints_require_direct_memory_accesses(self):
        result = compare.run_comparison(self.binary, self.bios, self.media, self.root / 'run',
                                        kind='state', modes=('recomp', 'recomp'), fastmem='lut',
                                        interval=2, checkpoints=2, min_blocks=10, timeout=30)
        self.assertTrue(result['passed'], result)
        self.assertTrue(all(run['exit_code'] == 0 and not run['timed_out']
                            for run in result['runs']), result)
        self.assertTrue(all(run.get('execution', {}).get('native_completed', 0) >= 10
                            for run in result['runs']), result)
        self.assertTrue(all(run['observed_configuration'] == [('recomp', 'lut', 'on', 'on')]
                            for run in result['runs']), result)
        self.assertTrue(all(run['lut']['loads'] > 0 and run['lut']['stores'] > 0 for run in result['runs']), result)

    def test_native_linked_mmap_checkpoints(self):
        self.run_pair(('recomp', 'recomp'), 'state', 'mmap')

    def test_native_lut_repeated_memory_accesses_survive_restore(self):
        self.write_program((0x3C08A003, 0x24090000, 0xAD090000, 0x8D0A0000, 0, 0x25290001, 0x1000FFFB, 0))
        for linking in (True, False):
            with self.subTest(linking=linking):
                result = compare.run_comparison(self.binary, self.bios, self.media,
                                                self.root / f'lut-restore-{linking}', kind='state',
                                                modes=('recomp', 'recomp'), fastmem='lut', linking=linking,
                                                interval=2, checkpoints=2, min_blocks=10, restore_at=1, timeout=30)
                self.assertTrue(result['passed'], result)
                self.assertTrue(all(run['lut']['loads'] > 0 and run['lut']['stores'] > 0
                                    for run in result['runs']), result)

    def test_disabled_native_instruction_cache_cannot_qualify(self):
        directory = self.root / 'disabled-cache'
        directory.mkdir()
        (directory / 'data').symlink_to(compare.ROOT / 'data', target_is_directory=True)
        env = {key: value for key, value in os.environ.items() if not key.startswith('CUPID_')}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CUPID_GPU_SW_ISA='SCALAR',
                   CUPID_CPU_STATS='1', CUPID_CHECKPOINT_DIR=str(directory), CUPID_CHECKPOINT_SCHEDULE='2,2')
        command = [str(self.binary), '--cpu-mode', 'recomp', '--fastmem', 'off', '--no-icache',
                   '--gpu-renderer', 'software', '--pgxp', 'off', '--bios', str(self.bios), str(self.media)]
        result = compare.wait_child(command, directory, env, 30)
        compare.inspect_run(directory, result, 'state', 'recomp', 1000, 2, 2, 10, 'off', True)
        self.assertEqual(result['exit_code'], 0, result)
        self.assertGreaterEqual(result['execution']['native_completed'], 10, result)
        self.assertEqual(result['observed_configuration'], [('recomp', 'off', 'on', 'off')])
        self.assertFalse(result['passed'], result)
        self.assertEqual(result['failures'], ['missing, repeated or unexpected execution configuration'])

    def test_saved_machine_continues_identically_after_restore(self):
        for mode, fastmem in (('interp', 'off'), ('cached', 'off'),
                              ('recomp', 'off'), ('recomp', 'mmap')):
            with self.subTest(mode=mode, fastmem=fastmem):
                result = compare.run_comparison(self.binary, self.bios, self.media,
                                                self.root / ('restore-' + mode + '-' + fastmem),
                                                kind='state', modes=(mode, mode), fastmem=fastmem,
                                                interval=2, checkpoints=3, restore_at=1,
                                                min_blocks=10, timeout=30)
                self.assertTrue(result['passed'], result)

    def test_gte_and_divide_loop_continues_identically_after_restore(self):
        # Enable COP2, loop through independent divide/GTE work and store results.
        self.write_program((0x3C084000, 0x40886000, 0x3C10A003, 0x24083039, 0x24090007,
                            0x0109001A, 0x4A000030, 0x480AC000, 0x00005812,
                            0xAE0B0000, 0x1000FFFA, 0))
        self.test_saved_machine_continues_identically_after_restore()

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
