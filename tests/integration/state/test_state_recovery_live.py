"""Rejected foreign states preserve active CPU, audio and device continuation."""
import os
from pathlib import Path
import re
import shutil
import struct
import tempfile
import unittest

from ..guest_program import write_inputs
from ..gpu.test_gpu_rectangle_live import compare
from ..spu import state_program as guest
from ..spu import test_spu_state_live as audio_tests


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class StateRecoveryLiveTest(unittest.TestCase):
    def run_guest(self, binary, bios, exe, directory, mode, memory, linking, bad=None):
        directory.mkdir()
        if os.name == 'nt': shutil.copytree(compare.ROOT / 'data', directory / 'data')
        else: (directory / 'data').symlink_to(compare.ROOT / 'data', target_is_directory=True)
        command = [str(binary), '--cpu-mode', mode, '--fastmem', memory,
                   '--gpu-renderer', 'software', '--pgxp', 'off', '--bios', str(bios)]
        if not linking: command.append('--no-block-linking')
        command.append(str(exe))
        env = {key: value for key, value in os.environ.items() if not key.startswith('CUPID_')}
        env.update(SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', CUPID_GPU_SW_ISA='SCALAR',
                   CUPID_CPU_STATS='1', CUPID_CHECKPOINT_DIR=str(directory), CUPID_CHECKPOINT_SCHEDULE='3,2')
        if bad:
            env.update(CUPID_CHECKPOINT_RESTORE='1', CUPID_CHECKPOINT_REJECT_FILE=str(bad))
        result = compare.wait_child(command, directory, env, 30)
        compare.inspect_run(directory, result, 'state', mode, 1, 3, 2, 10, memory, linking)
        self.assertTrue(result['passed'], (directory, result))
        if mode == 'cached': self.assertEqual(result['cached']['fallback'], 0)
        if mode == 'recomp':
            self.assertEqual(result['execution']['interpreted'], result['execution']['native_dispatch_delay_slots'])
            if memory == 'lut':
                self.assertGreater(result['lut']['loads'], 0)
                self.assertGreater(result['lut']['stores'], 0)
        log = (directory / 'boot.log').read_text(errors='replace')
        reports = list(re.finditer(r'^\[execution-checkpoint\] rejected index=(\d+) frame=(\d+) ticks=(\d+) '
                                  r'error=(.*)$', log, re.MULTILINE))
        self.assertEqual(log.count('[execution-checkpoint] rejected'), int(bad is not None))
        self.assertEqual(len(reports), int(bad is not None))
        if bad:
            frame, ticks = result['checkpoints'][0]
            self.assertEqual(reports[0].groups(), ('1', str(frame), str(ticks), 'save-state stream is corrupted.'))
            self.assertLess(log.index(f'[execution-checkpoint] frame={frame} ticks={ticks}'), reports[0].start())
            later_frame, later_ticks = result['checkpoints'][1]
            self.assertLess(reports[0].start(), log.index(f'[execution-checkpoint] frame={later_frame} ticks={later_ticks}'))
            before, after = directory / 'checkpoint-00000003.sav', directory / 'rejected-00000003.sav'
            compare.validate_checkpoint(after)
            self.assertIsNone(compare.first_difference(before, after), directory)
        return result

    def malformed_foreign_state(self, source, target, cue, late):
        compare.validate_checkpoint(source)
        data = bytearray(source.read_bytes())
        self.assertEqual(struct.unpack_from('<I', data, 168)[0], 0)
        if late:
            marker = struct.pack('<I', 7) + b'Cheevos'
            self.assertEqual(data.count(marker), 1)
            data[data.index(marker) + 4] = ord('X')
        else:
            cd, pad = struct.pack('<I', 5) + b'CDROM', struct.pack('<I', 3) + b'Pad'
            self.assertEqual(data.count(cd), 1)
            self.assertEqual(data.count(pad), 1)
            end = data.index(pad)
            self.assertGreater(end, data.index(cd) + 81)
            self.assertEqual(data[end - 81:end - 1], bytes(80))
            struct.pack_into('<I', data, end - 81, 8)
        offset = struct.unpack_from('<I', data, 212)[0]
        path = str(cue).encode('utf-8')
        header = data[:216]
        struct.pack_into('<3I', header, 168, len(path), 216, 0)
        struct.pack_into('<I', header, 212, 216 + len(path))
        target.write_bytes(header + path + data[offset:])
        compare.validate_checkpoint(target)

    def test_rejected_foreign_state_preserves_execution(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        configs = [('interp', 'off', True), ('cached', 'off', True)]
        configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            disc = bytearray(64 * 2352)
            for sector in range(64):
                start = sector * 2352
                disc[start + 1:start + 11] = b'\xff' * 10
                disc[start + 13:start + 16] = bytes((2, (sector // 10) * 16 + sector % 10, 2))
            (root / 'foreign.bin').write_bytes(disc)
            cue = root / 'foreign.cue'
            cue.write_text('FILE "foreign.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n')
            audio = audio_tests.SpuStateLiveTest()
            for pal in (False, True):
                bios, exe = root / f'generated-{pal}.bin', root / f'generated-{pal}.exe'
                write_inputs(bios, exe, guest.build(pal))
                for mode, memory, linking in configs:
                    with self.subTest(pal=pal, mode=mode, memory=memory, linking=linking):
                        output = root / f'{pal}-{mode}-{memory}-{linking}'
                        output.mkdir()
                        left = output / 'control'
                        expected = self.run_guest(binary, bios, exe, left, mode, memory, linking)
                        for frame, _ in expected['checkpoints']:
                            audio.check_result(left / f'checkpoint-{frame:08d}.sav', pal)
                        for late in (False, True):
                            bad = output / f'bad-{late}.sav'
                            self.malformed_foreign_state(left / 'checkpoint-00000006.sav', bad, cue, late)
                            digest = compare.sha256(bad)
                            right = output / f'rejected-{late}'
                            observed = self.run_guest(binary, bios, exe, right, mode, memory, linking, bad)
                            self.assertEqual(expected['checkpoints'], observed['checkpoints'])
                            self.assertEqual(compare.sha256(bad), digest)
                            for frame, _ in expected['checkpoints']:
                                name = f'checkpoint-{frame:08d}.sav'
                                self.assertIsNone(compare.first_difference(left / name, right / name), (right, name))


if __name__ == '__main__': unittest.main()
