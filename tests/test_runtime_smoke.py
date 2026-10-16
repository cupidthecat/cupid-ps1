"""Check that the runtime runner rejects false success and enforces its deadline."""

import importlib.util
import os
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "runtime_smoke", Path(__file__).resolve().parents[1] / "scripts/runtime_smoke.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


@unittest.skipUnless(os.name == "posix", "Linux runtime runner")
class RuntimeSmokeTest(unittest.TestCase):
    def check_child(self, body, timeout=5):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary = root / "fake-emulator"
            binary.write_text("#!/usr/bin/env python3\nimport os, pathlib, time\n" + body,
                              encoding="utf-8")
            binary.chmod(0o700)
            bios = root / "bios"
            media = root / "media"
            bios.write_bytes(b"generated test input")
            media.write_bytes(b"generated test input")
            return smoke.run_smoke(binary, bios, media, "interp", root / "run", timeout)

    @staticmethod
    def completion():
        return (
            "pathlib.Path(os.environ['CUPID_RT_PATH']).write_bytes(b'state')\n"
            "print('[rt-test] save+load round-trip OK at frame 300', flush=True)\n"
            "print('[rt-test] continuation OK at frame 360', flush=True)\n"
            f"print({smoke.PASS_MARKER!r}, flush=True)\n"
        )

    def test_complete_exit(self):
        result = self.check_child(self.completion())
        self.assertTrue(result["passed"])
        self.assertEqual(result["exit_code"], 0)

    def test_completion_followed_by_failure(self):
        result = self.check_child(self.completion() + "raise SystemExit(3)\n")
        self.assertFalse(result["passed"])
        self.assertEqual(result["exit_code"], 3)

    def test_completion_followed_by_hang(self):
        # Even graceful termination after the deadline must fail validation.
        handler = "import signal, sys\nsignal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n"
        result = self.check_child(handler + self.completion() + "time.sleep(30)\n", timeout=1)
        self.assertFalse(result["passed"])
        self.assertTrue(result["timed_out"])
        self.assertEqual(result["exit_code"], 0)

    def test_marker_without_save_load(self):
        result = self.check_child(f"print({smoke.PASS_MARKER!r})\n")
        self.assertFalse(result["passed"])

    def test_repeated_completion(self):
        result = self.check_child(self.completion() + f"print({smoke.PASS_MARKER!r})\n")
        self.assertFalse(result["passed"])

    def test_missing_state(self):
        body = self.completion().split("\n", 1)[1]
        result = self.check_child(body)
        self.assertFalse(result["passed"])

    def test_insufficient_continuation(self):
        result = self.check_child(self.completion().replace("frame 360", "frame 359"))
        self.assertFalse(result["passed"])


if __name__ == "__main__":
    unittest.main()
