"""Check immutable old GPU bytes and the exact original synthetic stimulus."""
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest
import zlib

from .integration.gpu import legacy_polyline_program
from .integration.guest_program import write_inputs


class GpuLegacyFixtureTest(unittest.TestCase):
    def test_record_and_stimulus_hashes(self):
        folder = Path(__file__).parent / 'core/gpu/fixtures'
        metadata = json.loads((folder / 'polyline_v93.json').read_text())
        self.assertEqual(metadata['version'], 93)
        self.assertTrue(metadata['synthetic_inputs_only'])
        header = (folder / 'polyline_v93.h').read_text()
        for record in metadata['records']:
            with self.subTest(record=record['name']):
                match = re.search(r'const u8 ' + record['name'] + r'\[\] = \{(.*?)\};', header, re.S)
                self.assertIsNotNone(match)
                compressed = bytes(int(byte, 16) for byte in re.findall(r'0x([0-9A-F]{2})', match.group(1)))
                raw = zlib.decompress(compressed)
                self.assertEqual(len(compressed), record['compressed_bytes'])
                self.assertEqual(len(raw), record['uncompressed_bytes'])
                self.assertEqual(hashlib.sha256(compressed).hexdigest(), record['compressed_sha256'])
                self.assertEqual(hashlib.sha256(raw).hexdigest(), record['sha256'])
        with tempfile.TemporaryDirectory() as temporary:
            bios, exe = Path(temporary) / 'synthetic.bin', Path(temporary) / 'synthetic.exe'
            for stimulus in metadata['stimuli']:
                write_inputs(bios, exe, legacy_polyline_program.build(stimulus['shaded']))
                self.assertEqual(hashlib.sha256(bios.read_bytes()).hexdigest(), stimulus['firmware_sha256'])
                self.assertEqual(hashlib.sha256(exe.read_bytes()).hexdigest(), stimulus['executable_sha256'])


if __name__ == '__main__': unittest.main()
