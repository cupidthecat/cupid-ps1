#!/usr/bin/env python3
"""Run a bounded save/load smoke check with local BIOS and media inputs."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
PASS_MARKER = "[rt-test] PASS - save/load, continuation and shutdown complete"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run_smoke(binary, bios, media, mode, output, timeout, revision=None, hash_inputs=()):
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("timeout must be finite and positive")
    paths = [Path(p).resolve(strict=True) for p in (binary, bios, media, *hash_inputs)]
    if not all(path.is_file() for path in paths):
        raise ValueError("binary and input paths must be regular files")
    binary, bios, media = paths[:3]
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "data").symlink_to(ROOT / "data", target_is_directory=True)
    state = output / "roundtrip.sav"
    command = [str(binary), "--cpu-mode", mode, "--fastmem", "off",
               "--gpu-renderer", "software", "--pgxp", "off",
               "--bios", str(bios), "--test-savestate-roundtrip", str(media)]
    env = {key: value for key, value in os.environ.items() if not key.startswith("CUPID_")}
    env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy",
               CUPID_GPU_SW_ISA="SCALAR", CUPID_RT_PATH=str(state))
    result = {
        "revision": revision,
        "command": command,
        "sha256": {str(path): sha256(path) for path in paths},
        "environment": {key: env[key] for key in
                        ("SDL_VIDEODRIVER", "SDL_AUDIODRIVER", "CUPID_GPU_SW_ISA", "CUPID_RT_PATH")},
        "timeout_seconds": timeout,
        "timed_out": False,
    }
    started = time.monotonic()
    with (output / "boot.log").open("wb") as log:
        child = subprocess.Popen(command, cwd=output, env=env, stdout=log,
                                 stderr=subprocess.STDOUT, start_new_session=True)
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            result["timed_out"] = True
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(child.pid, signal.SIGKILL)
                child.wait()
    result["elapsed_seconds"] = time.monotonic() - started
    result["exit_code"] = child.returncode
    log = (output / "boot.log").read_text(encoding="utf-8", errors="replace")
    saved = re.findall(r"\[rt-test\] save\+load round-trip OK at frame (\d+)", log)
    continued = re.findall(r"\[rt-test\] continuation OK at frame (\d+)", log)
    result["save_load_frames"] = [int(frame) for frame in saved]
    result["continuation_frames"] = [int(frame) for frame in continued]
    result["completion_count"] = log.count(PASS_MARKER)
    result["state_bytes"] = state.stat().st_size if state.is_file() else 0
    result["passed"] = (
        not result["timed_out"] and child.returncode == 0
        and len(saved) == 1 and int(saved[0]) >= 300
        and len(continued) == 1 and int(continued[0]) >= int(saved[0]) + 60
        and result["completion_count"] == 1 and result["state_bytes"] > 0
        and "[rt-test] FAIL" not in log
    )
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", type=Path)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/cupid-ps1")
    parser.add_argument("--mode", choices=("interp", "cached", "recomp"), default="interp")
    parser.add_argument("--output", type=Path, help="new directory for private runtime files")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--revision", help="tested source revision, recorded with binary/input hashes")
    parser.add_argument("--hash-input", type=Path, action="append", default=[],
                        help="also hash a referenced BIN, playlist entry or other input")
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("run this script in the supported Linux build environment")
    output = args.output or ROOT / ".local-research" / (
        "runtime-" + args.mode + "-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    try:
        result = run_smoke(args.binary, args.bios, args.media, args.mode, output,
                           args.timeout, args.revision, args.hash_input)
    except (OSError, ValueError) as error:
        parser.exit(2, f"runtime smoke failed: {error}\n")
    print(f"{'PASS' if result['passed'] else 'FAIL'}: {output.resolve() / 'result.json'}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
