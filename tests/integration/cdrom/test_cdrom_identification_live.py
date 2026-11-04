"""Generated audio/Mode1/Mode2 discs through guest MMIO and restored state."""
import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

from . import identification_program as guest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('cd_id_compare', ROOT / 'scripts/compare_execution.py')
compare = importlib.util.module_from_spec(spec); spec.loader.exec_module(compare)


@unittest.skipUnless(os.environ.get('CUPID_TEST_BINARY'), 'set CUPID_TEST_BINARY for generated guest boots')
class CdIdentificationLiveTest(unittest.TestCase):
    def test_disc_types_and_restored_continuation(self):
        binary = Path(os.environ['CUPID_TEST_BINARY']).resolve(strict=True)
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            bios = directory / 'cd-id-\u732b.bin'
            words = guest.build()
            firmware = bytearray(512 * 1024)
            struct.pack_into('<' + 'I' * len(words), firmware, 0, *words)
            bios.write_bytes(firmware)
            (directory / 'sectors.bin').write_bytes(bytes(32 * 2352))
            configs = [('interp', 'off', True), ('cached', 'off', True)]
            configs += [('recomp', memory, linking) for memory in ('off', 'lut', 'mmap') for linking in (True, False)]
            for label, mode_text, disc_type, flags in [('audio', 'AUDIO', 0, 0x90),
                                                     ('mode1', 'MODE1/2352', 0, 0x80),
                                                     ('mode2', 'MODE2/2352', 0x20, 0x80)]:
                media = directory / (label + '-\u03bb.cue')
                media.write_text(f'FILE "sectors.bin" BINARY\n  TRACK 01 {mode_text}\n    INDEX 01 00:00:00\n')
                for mode, fastmem, linking in configs:
                    with self.subTest(disc=label, mode=mode, fastmem=fastmem, linking=linking):
                        output = directory / f'{label}-{mode}-{fastmem}-{linking}'
                        result = compare.run_comparison(binary, bios, media, output,
                            kind='state', modes=(mode, mode), interval=18, checkpoints=2, min_blocks=100,
                            fastmem=fastmem, linking=linking, restore_at=1, timeout=45)
                        self.assertTrue(result['passed'], result)
                        for side, run in zip(('left', 'right'), result['runs']):
                            if mode == 'recomp':
                                self.assertEqual(run['execution']['interpreted'], run['execution']['native_dispatch_delay_slots'])
                                if fastmem == 'lut':
                                    self.assertGreater(run['lut']['loads'], 0)
                                    self.assertGreater(run['lut']['stores'], 0)
                            elif mode == 'cached': self.assertEqual(run['cached']['fallback'], 0)
                            for frame, _ in run['checkpoints']:
                                self.check_result(output / side / f'checkpoint-{frame:08d}.sav', disc_type, flags)

    def check_result(self, path, disc_type, flags):
        data = path.read_bytes()
        self.assertEqual(struct.unpack_from('<2I', data), (0x43435544, 97))
        compression, size, unpacked, offset = struct.unpack_from('<4I', data, 200)
        self.assertEqual(compression, 0); self.assertEqual(size, unpacked)
        self.assertEqual(offset + size, len(data))
        payload = data[offset:]
        marker = b'\x03\0\0\0Bus'; self.assertEqual(payload.count(marker), 1)
        start = payload.index(marker) + len(marker) + 4 + 5 * 3 * 4 + guest.RESULT_ADDRESS
        self.assertEqual(struct.unpack_from('<4I', payload, start), (guest.COMPLETE, 0x0A, flags, disc_type))
        self.assertEqual(payload[start + 16:start + 24], bytes([0x0A, flags, disc_type, 0, 0, 0, 0, 0]))
        self.assertGreater(struct.unpack_from('<I', payload, start + 32)[0], 0)


if __name__ == '__main__': unittest.main()
