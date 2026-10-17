"""Check help and argument errors without starting the emulator."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


@unittest.skipUnless(os.environ.get("CUPID_TEST_BINARY"), "set CUPID_TEST_BINARY to check the frontend")
class FrontendCliTest(unittest.TestCase):
    def setUp(self):
        self.binary = Path(os.environ["CUPID_TEST_BINARY"]).resolve(strict=True)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.env = {key: value for key, value in os.environ.items() if not key.startswith("CUPID_")}
        # Any accidental SDL startup must fail, even on a machine with a display.
        self.env.update(SDL_VIDEODRIVER="cupid-cli-no-video", SDL_AUDIODRIVER="cupid-cli-no-audio")

    def invoke(self, arguments):
        before = {path.relative_to(self.directory): path.read_bytes()
                  for path in self.directory.rglob("*") if path.is_file()}
        before_paths = {path.relative_to(self.directory) for path in self.directory.rglob("*")}
        result = subprocess.run([str(self.binary), *arguments], cwd=self.directory,
                                env=self.env, capture_output=True, text=True, timeout=10)
        self.assertEqual({path.relative_to(self.directory) for path in self.directory.rglob("*")}, before_paths)
        self.assertEqual({path.relative_to(self.directory): path.read_bytes()
                          for path in self.directory.rglob("*") if path.is_file()}, before)
        return result

    def check_help(self, arguments):
        result = self.invoke(arguments)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(result.stdout.startswith("Usage:"), result.stdout)
        self.assertIn("--cpu-mode", result.stdout)
        self.assertEqual(result.stderr, "")
        return result.stdout

    def check_error(self, arguments, message):
        result = self.invoke(arguments)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn(message, result.stderr)
        for marker in ("[settings]", "[cpu-stats]", "[rt-test]", "debug ringbuffer dump", "SDL"):
            self.assertNotIn(marker, result.stderr)

    def test_short_and_long_help(self):
        for flag in ("-h", "--help"):
            with self.subTest(flag=flag):
                self.check_help([flag])

    def test_help_after_valid_options(self):
        options = (["--cpu-mode", "recomp"], ["--cpu-mode=cached"],
                   ["--gpu-renderer", "opengl"], ["--gpu-renderer=automatic"],
                   ["--gpu-resolution-scale", "16"], ["--fastmem=lut"], ["--pgxp=off"],
                   ["--no-block-linking", "--no-icache", "--cdrom-precache"],
                   ["--bios", "absent.bin", "absent.cue"],
                   ["--bios", "old.bin", "--bios", "replacement.bin"])
        for prefix in options:
            for flag in ("-h", "--help"):
                with self.subTest(prefix=prefix, flag=flag):
                    self.check_help([*prefix, flag])

    def test_help_does_not_read_or_write_settings(self):
        (self.directory / "settings.ini").write_text("[CPU]\nExecutionMode = Interpreter\n", encoding="utf-8")
        for flag in ("-h", "--help"):
            with self.subTest(flag=flag):
                text = self.check_help([flag])
                self.assertIn("default: recomp", text)
                self.assertIn("settings.ini", text)
                self.assertIn("experimental", text)
                self.assertNotIn("factory lands", text)
                self.assertNotIn("default: interp", text)

    def test_help_stops_at_its_position(self):
        self.check_help(["--help", "--unknown", "one.cue", "two.cue"])
        self.check_error(["--unknown", "--help"], "unknown option")
        self.check_error(["--cpu-mode", "wrong", "--help"], "unknown cpu mode")
        self.check_error(["one.cue", "two.cue", "--help"], "more than one game path")

    def test_missing_option_values(self):
        for option in ("--bios", "--cpu-mode", "--gpu-renderer", "--gpu-resolution-scale", "--fastmem", "--pgxp"):
            with self.subTest(option=option):
                self.check_error([option], f"{option} requires an argument")

    def test_invalid_option_values(self):
        cases = (("--cpu-mode", "unknown", "unknown cpu mode"),
                 ("--gpu-renderer", "unknown", "unknown renderer"),
                 ("--fastmem", "unknown", "unknown fastmem mode"),
                 ("--pgxp", "unknown", "expects on|off"))
        for option, value, message in cases:
            for arguments in ([option, value], [f"{option}={value}"], [f"{option}="]):
                with self.subTest(arguments=arguments):
                    self.check_error(arguments, message)

    def test_invalid_resolution_scales(self):
        for value in ("", "0", "17", "-1", "1.5", "one", "2x", "999999999999999999999999"):
            for arguments in (["--gpu-resolution-scale", value], [f"--gpu-resolution-scale={value}"]):
                with self.subTest(arguments=arguments):
                    self.check_error(arguments, "must be an integer 1..16")

    def test_missing_and_extra_game_arguments(self):
        self.check_error([], "no game image specified")
        self.check_error(["--cpu-mode", "interp"], "no game image specified")
        self.check_error(["one.cue", "two.cue"], "more than one game path")
        self.check_error(["absent.cue"], "no BIOS specified")
        self.check_error(["--bios", "absent.bin", "absent.cue"], "not found")
        self.check_error(["--unknown"], "unknown option")


if __name__ == "__main__":
    unittest.main()
