"""Generated firmware reaches the host EXE injection hook with known registers."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from ..guest_program import Program

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('exe_stack_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)

CALLER_SP, CALLER_FP, CALLER_GP = 0x801F9000, 0xA0A10000, 0xABCD0000
ENTRY_GP, RESULT, COMPLETE = 0x13572468, 0x30000, 0x45584543
CASES = [(0, 0), (0, 0x100), (0, 0xFFFFFFFF),
         (0x801FC000, 0), (0x801FC000, 0x28), (0xFFFFFFF0, 0x10)]


def write_inputs(bios, executable, base, offset):
    boot = Program()
    boot.constant(29, CALLER_SP); boot.constant(30, CALLER_FP); boot.constant(28, CALLER_GP)
    boot.constant(8, 0x1F802041); boot.constant(9, 7)
    boot.emit(0xA1090000)  # POST 07 invokes the executable loader.
    boot.label('failed'); boot.branch(4, 0, 0, 'failed')
    words = boot.finish()
    firmware = bytearray(512 * 1024)
    struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
    bios.write_bytes(firmware)

    entry = Program()
    entry.constant(8, 0xA0000000 | RESULT)
    entry.emit(0xAD1D0000, 0xAD1E0004, 0xAD1C0008)  # Save SP, FP and GP without using the stack.
    entry.constant(9, COMPLETE); entry.emit(0xAD09000C)
    entry.label('loop'); entry.emit(0x8D0A000C, 0, 0xAD0A0010)
    entry.branch(4, 0, 0, 'loop')
    words = entry.finish()
    image = bytearray(2048 + len(words) * 4)
    image[:8] = b'PS-X EXE'
    struct.pack_into('<4I', image, 16, 0x80010000, ENTRY_GP, 0x80010000, len(words) * 4)
    struct.pack_into('<2I', image, 0x30, base, offset)
    struct.pack_into('<' + 'I' * len(words), image, 2048, *words)
    executable.write_bytes(image)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class ExecutableStackLiveTest(unittest.TestCase):
    def test_entry_registers_and_restored_continuation(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bios, media = directory / 'boot-\u732b.bin', directory / 'entry-\u03bb.exe'
            for case, (base, offset) in enumerate(CASES):
                write_inputs(bios, media, base, offset)
                stack = (base + offset) & 0xFFFFFFFF
                expected = (stack if base else CALLER_SP, stack if base else CALLER_FP,
                            ENTRY_GP, COMPLETE, COMPLETE)
                for mode, fastmem, linking in configs:
                    with self.subTest(case=case, mode=mode, fastmem=fastmem, linking=linking):
                        output = directory / f'{case}-{mode}-{fastmem}-{linking}'
                        result = compare.run_comparison(binary, bios, media, output,
                            kind='state', modes=(mode, mode), interval=2, checkpoints=2, min_blocks=10,
                            fastmem=fastmem, linking=linking, restore_at=1, timeout=30)
                        self.assertTrue(result['passed'], result)
                        for side, run in zip(('left', 'right'), result['runs']):
                            self.assertIn('Injected EXE:', (output / side / 'boot.log').read_text(errors='replace'))
                            if mode == 'recomp':
                                self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                                if fastmem == 'lut':
                                    self.assertGreater(run['lut']['loads'], 0)
                                    self.assertGreater(run['lut']['stores'], 0)
                            elif mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                            for frame, _ in run['checkpoints']:
                                data = (output / side / f'checkpoint-{frame:08d}.sav').read_bytes()
                                self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 93))
                                compression, size, unpacked, start = struct.unpack_from('<4I', data, 200)
                                self.assertEqual(compression, 0); self.assertEqual(size, unpacked)
                                self.assertEqual(start + size, len(data))
                                payload = data[start:]
                                marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
                                ram = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4
                                self.assertEqual(struct.unpack_from('<5I', payload, ram + RESULT), expected)


if __name__ == '__main__': unittest.main()
