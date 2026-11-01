"""Exercise guest SIO0 transactions and card persistence across real processes."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import tempfile
import unittest
from . import card_program

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('card_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class MemoryCardLiveTest(unittest.TestCase):
    def test_guest_protocol_and_persistent_reload(self):
        with tempfile.TemporaryDirectory() as temporary:
            self.run_cases(Path(temporary))

    def test_deferred_flush_precedes_shutdown(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            shutil.copytree(ROOT / 'data', directory / 'data')
            binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
            self.boot(binary, directory, 'recomp', 'lut', True, interval=360)
            log = (directory / 'boot.log').read_text(errors='replace')
            self.assertEqual(log.count('Saving memory card to'), 1)
            self.assertLess(log.index('Saving memory card to'), log.index('[execution-checkpoint] frame=360'))
            image = (directory / 'memcards/card1.mcd').read_bytes()
            start = card_program.SECTOR * 128
            self.assertEqual(image[start:start + 128], card_program.CONTENT)

    def run_cases(self, root):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        for mode, fastmem in [('interp', 'off'), ('cached', 'off'), ('recomp', 'off'),
                              ('recomp', 'lut'), ('recomp', 'mmap')]:
            with self.subTest(mode=mode, fastmem=fastmem):
                directory = root / (mode + '-' + fastmem)
                directory.mkdir()
                shutil.copytree(ROOT / 'data', directory / 'data')
                card = directory / 'memcards/card1.mcd'
                self.boot(binary, directory, mode, fastmem, True)
                contents = card.read_bytes()
                self.assertEqual(len(contents), 128 * 1024)
                self.assertEqual(contents[:2], b'MC')
                start = card_program.SECTOR * 128
                self.assertEqual(contents[start:start + 128], card_program.CONTENT)
                digest = hashlib.sha256(contents).digest()
                self.assertEqual(sorted(path.name for path in card.parent.iterdir()), ['card1.mcd'])
                self.boot(binary, directory, mode, fastmem, False)
                self.assertEqual(hashlib.sha256(card.read_bytes()).digest(), digest)

    def boot(self, binary, directory, mode, fastmem, writer, *, interval=12):
        label = 'write' if writer else 'read'
        words, expected = card_program.build(writer)
        bios, executable = (directory / (label + suffix) for suffix in ('-\u732b.bin', '-\u03bb.exe'))
        card_program.write_inputs(bios, executable, words)
        checkpoints = directory / label
        checkpoints.mkdir()
        env = {key: value for key, value in os.environ.items() if not key.startswith('CUPID_')}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CUPID_GPU_SW_ISA='SCALAR',
                   CUPID_CPU_STATS='1', CUPID_CHECKPOINT_DIR=str(checkpoints), CUPID_CHECKPOINT_SCHEDULE=f'{interval},2')
        command = [str(binary), '--cpu-mode', mode, '--fastmem', fastmem, '--gpu-renderer', 'software',
                   '--pgxp', 'off', '--bios', str(bios), str(executable)]
        # The same working directory is reopened, so the second process loads the card it sees on disk.
        result = compare.wait_child(command, directory, env, 45)
        log = (directory / 'boot.log').read_text(errors='replace')
        (checkpoints / 'boot.log').write_text(log)
        self.assertEqual(result['exit_code'], 0, result)
        self.assertFalse(result['timed_out'], result)
        self.assertEqual(log.count('[execution-checkpoint] completed count=2'), 1, log[-3000:])
        for frame in (interval, interval * 2):
            data = (checkpoints / f'checkpoint-{frame:08d}.sav').read_bytes()
            compression, size, unpacked, offset = struct.unpack_from('<4I', data, 200)
            self.assertEqual(compression, 0)
            self.assertEqual(size, unpacked)
            self.assertEqual(offset + size, len(data))
            payload = data[offset:]
            marker = b'\x03\0\0\0Bus'
            self.assertEqual(payload.count(marker), 1)
            bus = payload.index(marker) + len(marker)
            ram_size = struct.unpack_from('<I', payload, bus)[0]
            self.assertIn(ram_size, (2 * 1024 * 1024, 8 * 1024 * 1024))
            start = bus + 4 + 5 * 3 * 4 + card_program.RESULT_ADDRESS
            self.assertEqual(struct.unpack_from('<2I', payload, start),
                             (card_program.RESULT_MAGIC, len(expected)), (mode, fastmem, label, frame))
            actual = payload[start + 16:start + 16 + len(expected)]
            if actual != expected:
                first = next(i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1])
                self.fail(f'{mode}/{fastmem}/{label} frame {frame}: response byte {first}: '
                          f'{actual[first]:02x} != {expected[first]:02x}')
        if mode == 'recomp':
            self.assertRegex(log, r'native_completed=[1-9][0-9]*')
        elif mode == 'cached':
            self.assertRegex(log, r'cached created=[1-9][0-9]* hits=[1-9][0-9]* blocks=[1-9][0-9]*')


if __name__ == '__main__':
    unittest.main()
