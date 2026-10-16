"""Regressions for false success in execution comparisons."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("compare_execution", Path(__file__).resolve().parents[1] / "scripts/compare_execution.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


@unittest.skipUnless(os.name == "posix", "Linux process runner")
class ExecutionComparisonTest(unittest.TestCase):
    def run_fake(self, change="", *, kind="trace", modes=("interp", "cached"), timeout=5, restore_at=0):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            binary, bios, media = (root / name for name in ("emulator", "bios", "media"))
            bios.write_bytes(b"generated")
            media.write_bytes(b"generated")
            code = f'''#!/usr/bin/env python3
import os, pathlib, struct, sys, time
mode = sys.argv[sys.argv.index('--cpu-mode') + 1]
right = pathlib.Path.cwd().name == 'right'
header = {compare.TRACE_HEADER!r}
rows = [str(i) + ' ' + ' '.join(['00000000'] * 116 + ['0000000000000000']) + '\\n' for i in range(1, 5)]
footer = '# completed 4\\n'
progress = "[cpu-stats] execution interpreted=100 native_entered=0 native_completed=0"
cached = "[cpu-stats] cached created=3 hits=30 blocks=50 instructions=100 fallback=0"
if mode == 'recomp':
    progress = "[cpu-stats] execution interpreted=0 native_entered=100 native_completed=100"
state_payload = b'generated-state'
restore = os.environ.get('CUPID_CHECKPOINT_RESTORE')
restore_report = f'[execution-checkpoint] restored index={{restore}} frame=2 ticks=200' if restore else ''
restore_after = False
icache = 'on'
{change}
if 'CUPID_DIFF_TRACE' in os.environ:
    pathlib.Path(os.environ['CUPID_DIFF_TRACE']).write_text(header + ''.join(rows) + footer)
else:
    directory = pathlib.Path(os.environ['CUPID_CHECKPOINT_DIR'])
    for frame in (2, 4):
        state = bytearray(216)
        struct.pack_into('<II', state, 0, 0x43435544, 84)
        struct.pack_into('<4I', state, 200, 0, len(state_payload), len(state_payload), 216)
        (directory / f'checkpoint-{{frame:08d}}.sav').write_bytes(state + state_payload)
        print(f'[execution-checkpoint] frame={{frame}} ticks={{frame * 100}}')
        if frame == 2 and restore_report and not restore_after:
            print(restore_report)
    if restore_report and restore_after:
        print(restore_report)
    print('[execution-checkpoint] completed count=2')
print(f'[cpu-stats] configuration mode={{mode}} fastmem=off linking=on icache={{icache}}')
print(progress)
print(cached)
'''
            binary.write_text(code, encoding="utf-8")
            binary.chmod(0o700)
            return compare.run_comparison(binary, bios, media, root / "run", kind=kind,
                                          modes=modes, instructions=4, interval=2, checkpoints=2,
                                          min_blocks=1, timeout=timeout, restore_at=restore_at)

    def test_complete_trace(self):
        self.assertTrue(self.run_fake()["passed"])

    def test_complete_state(self):
        self.assertTrue(self.run_fake(kind="state", modes=("interp", "recomp"))["passed"])

    def test_disabled_instruction_cache_cannot_qualify(self):
        result = self.run_fake("icache = 'off'", kind="state", modes=("recomp", "recomp"))
        self.assertFalse(result["passed"], result)
        self.assertTrue(all("unexpected execution configuration" in " ".join(run["failures"])
                            for run in result["runs"]), result)

    def test_complete_restored_continuation(self):
        result = self.run_fake(kind="state", modes=("recomp", "recomp"), restore_at=1)
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["runs"][0]["restores"], [])
        self.assertEqual(result["runs"][1]["restores"], [(1, 2, 200)])

    def test_missing_or_incorrect_restore_fails(self):
        for report in ("", "[execution-checkpoint] restored index=1 frame=2 ticks=199",
                       "[execution-checkpoint] restored index=2 frame=4 ticks=400",
                       "[execution-checkpoint] restored malformed",
                       "[execution-checkpoint] restored index=1 frame=2 ticks=200\n" * 2):
            with self.subTest(report=report):
                result = self.run_fake(f"if right: restore_report = {report!r}", kind="state",
                                       modes=("recomp", "recomp"), restore_at=1)
                self.assertFalse(result["passed"], result)
                self.assertTrue(result["runs"][0]["passed"], result)
                self.assertIn("restore report", " ".join(result["runs"][1]["failures"]))

    def test_late_restore_cannot_qualify_continuation(self):
        result = self.run_fake("restore_after = right", kind="state",
                               modes=("recomp", "recomp"), restore_at=1)
        self.assertFalse(result["passed"], result)
        self.assertIn("restore did not precede", " ".join(result["runs"][1]["failures"]))

    def test_unrequested_restore_fails(self):
        result = self.run_fake("restore_report = '[execution-checkpoint] restored index=1 frame=2 ticks=200'",
                               kind="state")
        self.assertFalse(result["passed"], result)

    def test_restore_requires_continuation_and_matching_modes(self):
        for configuration in ({"kind": "trace", "modes": ("interp", "interp"), "restore_at": 1},
                              {"kind": "state", "modes": ("interp", "recomp"), "restore_at": 1},
                              {"kind": "state", "restore_at": -1},
                              {"kind": "state", "modes": ("interp", "interp"), "restore_at": 5}):
            with self.subTest(configuration=configuration), self.assertRaises(ValueError):
                compare.run_comparison(__file__, __file__, __file__, "unused", **configuration)

    def test_startup_failure(self):
        self.assertFalse(self.run_fake("raise SystemExit(2)")["passed"])

    def test_empty_trace(self):
        self.assertFalse(self.run_fake("header = footer = ''; rows = []")["passed"])

    def test_short_trace(self):
        self.assertFalse(self.run_fake("rows = rows[:3]")["passed"])

    def test_wrong_sequence(self):
        self.assertFalse(self.run_fake("rows[1] = rows[0]")["passed"])

    def test_missing_footer(self):
        self.assertFalse(self.run_fake("footer = ''")["passed"])

    def test_extra_samples(self):
        self.assertFalse(self.run_fake("rows.append(rows[-1])")["passed"])

    def test_missing_counters(self):
        self.assertFalse(self.run_fake("progress = ''")["passed"])

    def test_no_cached_progress(self):
        self.assertFalse(self.run_fake("cached = '[cpu-stats] cached created=1 hits=0 blocks=0 instructions=0 fallback=99'")["passed"])

    def test_native_fallback_fails(self):
        self.assertFalse(self.run_fake("progress = '[cpu-stats] execution interpreted=999 native_entered=0 native_completed=0'",
                                       kind="state", modes=("interp", "recomp"))["passed"])

    def test_different_gpr_fails(self):
        result = self.run_fake("if right:\n    fields = rows[2].split(); fields[34] = '00000001'; rows[2] = ' '.join(fields) + '\\n'")
        self.assertFalse(result["passed"])
        self.assertTrue(result["differences"])

    def test_different_state_fails(self):
        result = self.run_fake("if right: state_payload = b'different-state'", kind="state")
        self.assertFalse(result["passed"])
        self.assertEqual(len(result["differences"]), 2)

    def test_deadline_fails_even_with_graceful_exit(self):
        result = self.run_fake("import signal\nsignal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\ntime.sleep(30)", timeout=0.2)
        self.assertFalse(result["passed"])
        self.assertTrue(all(run["timed_out"] for run in result["runs"]))

    def test_missing_inputs_fail(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileNotFoundError):
                compare.run_comparison(Path(temp) / "missing", __file__, __file__, Path(temp) / "run")

    def test_existing_output_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaises(FileExistsError):
                compare.run_comparison(__file__, __file__, __file__, temp)

    def test_truncated_checkpoint_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "state"
            path.write_bytes(b"state")
            with self.assertRaises(ValueError):
                compare.validate_checkpoint(path)

    def test_native_instruction_trace_rejected(self):
        with self.assertRaises(ValueError):
            compare.run_comparison(__file__, __file__, __file__, "unused", modes=("interp", "recomp"))


if __name__ == "__main__":
    unittest.main()
