#!/usr/bin/env python3
"""Compare bounded, isolated emulator runs and keep their evidence private."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
TRACE_HEADER = ("# cupid-cpu-trace-v2: sequence pc instruction gpr[34] cop0[11] gte[64] "
                "load_reg load_value next_load_reg next_load_value pending_ticks global_ticks\n")
COVERAGE = {
    "trace": {
        "compared": ["ordered pre-instruction PC and instruction", "all 32 GPRs, HI and LO",
                     "all 11 COP0 and 64 GTE words", "both pending load registers and values",
                     "CPU pending ticks and scheduler global ticks"],
        "excluded": ["other pipeline fields, cache control, cache tags/data and scratchpad",
                     "RAM, devices, scheduler event queue, external files and host state"],
        "boundary": "after instruction prefetch, before execution; completion follows the final instruction",
    },
    "state": {
        "compared": ["every byte of each uncompressed save-state file, without normalization",
                     "checkpoint frame number and scheduler global ticks"],
        "excluded": ["CPU host-dispatch flags/pointers and PGXP CPU shadow arrays not written by cpu_do_state",
                     "host mappings, generated native code, external BIOS/disc contents",
                     "host audio/video/input queues and asynchronous host reader state"],
        "serializer": "save_state_header_t and system_do_state format versions 84/85/86/87; see subsystem do_state functions",
        "boundary": "frontend frame checkpoint; CPU event overshoot and engine flags remain compared",
        "limits": "A difference can reflect engine bookkeeping or timing. Equality covers serialized state only.",
    },
}


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def wait_child(command, directory, env, timeout):
    result = {"command": command, "timed_out": False}
    started = time.monotonic()
    with (directory / "boot.log").open("wb") as log:
        try:
            child = subprocess.Popen(command, cwd=directory, env=env, stdout=log,
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
            result["exit_code"] = child.returncode
        except OSError as error:
            result.update(exit_code=None, error=str(error))
    result["elapsed_seconds"] = time.monotonic() - started
    return result


def validate_trace(path, count):
    """Require the exact schema, consecutive samples, a footer and EOF."""
    with path.open(encoding="ascii") as stream:
        if stream.readline() != TRACE_HEADER:
            raise ValueError("missing or unsupported trace header")
        for sequence in range(1, count + 1):
            fields = stream.readline().split()
            if (len(fields) != 118 or fields[0] != str(sequence)
                    or any(re.fullmatch(r"[0-9a-f]{8}", word) is None for word in fields[1:-1])
                    or re.fullmatch(r"[0-9a-f]{16}", fields[-1]) is None):
                raise ValueError(f"invalid or missing trace sample {sequence}")
        if stream.readline() != f"# completed {count}\n" or stream.read(1):
            raise ValueError("missing completion footer or unexpected trace data")


def validate_checkpoint(path):
    with path.open("rb") as stream:
        header = stream.read(216)
    if (len(header) != 216 or struct.unpack_from("<I", header)[0] != 0x43435544
            or struct.unpack_from("<I", header, 4)[0] not in (84, 85, 86, 87)):
        raise ValueError("missing or unsupported checkpoint header")
    compression, size, unpacked, offset = struct.unpack_from("<4I", header, 200)
    if compression != 0 or size == 0 or size != unpacked or offset < 216 or offset + size != path.stat().st_size:
        raise ValueError("invalid or truncated uncompressed checkpoint")


def first_difference(left, right):
    """Compare all bytes and retain the first differing offset, including EOF."""
    offset = 0
    with left.open("rb") as a, right.open("rb") as b:
        while True:
            x, y = a.read(65536), b.read(65536)
            if x != y:
                for index in range(min(len(x), len(y))):
                    if x[index] != y[index]:
                        return offset + index
                return offset + min(len(x), len(y))
            if not x:
                return None
            offset += len(x)


def inspect_run(directory, result, kind, mode, instructions, interval, checkpoints, min_blocks, fastmem, linking,
                restore_at=0):
    log = (directory / "boot.log").read_text(encoding="utf-8", errors="replace")
    failures = []
    if result["timed_out"] or result["exit_code"] != 0:
        failures.append("process did not complete normally before its deadline")
    configurations = re.findall(r"\[cpu-stats\] configuration mode=(interp|cached|recomp) "
                                 r"fastmem=(off|lut|mmap) linking=(on|off) icache=(on|off)", log)
    expected = (mode, fastmem if mode == "recomp" else "off", "on" if linking else "off", "on")
    if configurations != [expected]:
        failures.append("missing, repeated or unexpected execution configuration")
    result["observed_configuration"] = configurations
    execution = re.findall(r"\[cpu-stats\] execution interpreted=(\d+) native_entered=(\d+) native_completed=(\d+)", log)
    dispatch = re.findall(r"^\[cpu-stats\] native_dispatch delay_slots_interpreted=(\d+)$", log, re.MULTILINE)
    cached = re.findall(r"\[cpu-stats\] cached created=(\d+) hits=(\d+) blocks=(\d+) instructions=(\d+) fallback=(\d+)", log)
    if (len(execution) != 1 or len(cached) != 1 or len(dispatch) != 1
            or log.count('[cpu-stats] native_dispatch') != 1):
        failures.append("missing or repeated execution counters")
    else:
        result["execution"] = dict(zip(("interpreted", "native_entered", "native_completed"), map(int, execution[0])))
        result["execution"]["native_dispatch_delay_slots"] = int(dispatch[0])
        if int(dispatch[0]) > int(execution[0][0]):
            failures.append("native dispatch delay slots exceed interpreted instructions")
        result["cached"] = dict(zip(("created", "hits", "blocks", "instructions", "fallback"), map(int, cached[0])))
        if mode == "recomp":
            if min(map(int, execution[0][1:])) < min_blocks:
                failures.append("insufficient native block progress")
        elif int(execution[0][0]) < (instructions if kind == "trace" else 1):
            failures.append("insufficient interpreted instruction progress")
        if mode == "cached" and (min(map(int, cached[0][:4])) <= 0 or int(cached[0][2]) < min_blocks):
            failures.append("insufficient cached block creation, reuse or execution")
    artifacts = []
    try:
        if kind == "trace":
            trace = directory / "cpu.trace"
            validate_trace(trace, instructions)
            artifacts.append(trace)
        else:
            observed = [(int(frame), int(ticks)) for frame, ticks in re.findall(
                r"\[execution-checkpoint\] frame=(\d+) ticks=(\d+)", log)]
            result["checkpoints"] = observed
            if [frame for frame, _ in observed] != [interval * i for i in range(1, checkpoints + 1)]:
                raise ValueError("missing, repeated or out-of-order checkpoints")
            if any(b[1] <= a[1] for a, b in zip(observed, observed[1:])) or not observed[0][1]:
                raise ValueError("checkpoint ticks did not advance")
            if re.findall(r"\[execution-checkpoint\] completed count=(\d+)", log) != [str(checkpoints)]:
                raise ValueError("missing or repeated completion report")
            restores = list(re.finditer(r"^\[execution-checkpoint\] restored index=(\d+) frame=(\d+) ticks=(\d+)$",
                                        log, re.MULTILINE))
            result["restores"] = [tuple(map(int, match.groups())) for match in restores]
            expected = [(restore_at, *observed[restore_at - 1])] if restore_at else []
            if (result["restores"] != expected
                    or log.count("[execution-checkpoint] restored") != len(restores)):
                raise ValueError("missing, repeated or unexpected restore report")
            if restore_at:
                before, after = observed[restore_at - 1:restore_at + 1]
                if not (log.index(f"[execution-checkpoint] frame={before[0]} ticks={before[1]}")
                        < restores[0].start()
                        < log.index(f"[execution-checkpoint] frame={after[0]} ticks={after[1]}")):
                    raise ValueError("restore did not precede its continuation checkpoints")
            for frame, _ in observed:
                state = directory / f"checkpoint-{frame:08d}.sav"
                validate_checkpoint(state)
                artifacts.append(state)
    except (OSError, UnicodeError, ValueError) as error:
        failures.append(str(error))
    result["artifacts"] = {path.name: {"bytes": path.stat().st_size, "sha256": sha256(path)} for path in artifacts}
    result["failures"] = failures
    result["passed"] = not failures
    return artifacts


def run_comparison(binary, bios, media, output, *, kind="trace", modes=("interp", "cached"),
                   instructions=50000, interval=60, checkpoints=5, min_blocks=100,
                   fastmem="off", linking=True, timeout=60, revision=None, hash_inputs=(), restore_at=0):
    if (kind not in COVERAGE or len(modes) != 2 or any(mode not in ("interp", "cached", "recomp") for mode in modes)
            or (kind == "trace" and "recomp" in modes) or fastmem not in ("off", "lut", "mmap")):
        raise ValueError("unsupported comparison configuration")
    if (not math.isfinite(timeout) or timeout <= 0 or not 0 < instructions <= 0xffffffffffffffff or interval <= 0
            or not 1 <= checkpoints <= 32 or interval * checkpoints > 0xffffffff or min_blocks <= 0):
        raise ValueError("counts and timeout must be positive and within supported bounds")
    if restore_at and (kind != "state" or modes[0] != modes[1] or not 1 <= restore_at < checkpoints):
        raise ValueError("restore requires matching modes and a state checkpoint before the final checkpoint")
    paths = [Path(p).resolve(strict=True) for p in (binary, bios, media, *hash_inputs)]
    if not all(path.is_file() for path in paths):
        raise ValueError("binary and inputs must be regular files")
    binary, bios, media = paths[:3]
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    hashes = {str(path): sha256(path) for path in paths}
    result = {"revision": revision, "kind": kind, "coverage": COVERAGE[kind], "sha256": hashes,
              "configuration": {"modes": modes, "instructions": instructions, "interval": interval,
                                "checkpoints": checkpoints, "minimum_blocks": min_blocks,
                                "right_restore_at": restore_at,
                                "instruction_cache": True,
                                "fastmem": fastmem, "linking": linking, "timeout_seconds": timeout,
                                "timing": "native", "renderer": "scalar software", "pgxp": False},
              "runs": [], "differences": []}
    artifacts = []
    for label, mode in zip(("left", "right"), modes):
        directory = output / label
        directory.mkdir()
        (directory / "data").symlink_to(ROOT / "data", target_is_directory=True)
        command = [str(binary), "--cpu-mode", mode, "--fastmem", fastmem,
                   "--gpu-renderer", "software", "--pgxp", "off", "--bios", str(bios)]
        if not linking:
            command.append("--no-block-linking")
        command.append(str(media))
        env = {key: value for key, value in os.environ.items() if not key.startswith("CUPID_")}
        env.update(SDL_VIDEODRIVER="dummy", SDL_AUDIODRIVER="dummy", CUPID_GPU_SW_ISA="SCALAR", CUPID_CPU_STATS="1")
        if kind == "trace":
            env.update(CUPID_DIFF_TRACE=str(directory / "cpu.trace"), CUPID_DIFF_LIMIT=str(instructions))
        else:
            env.update(CUPID_CHECKPOINT_DIR=str(directory), CUPID_CHECKPOINT_SCHEDULE=f"{interval},{checkpoints}")
            if restore_at and label == "right":
                env["CUPID_CHECKPOINT_RESTORE"] = str(restore_at)
        observed = wait_child(command, directory, env, timeout)
        observed["environment"] = {key: env[key] for key in env if key.startswith(("CUPID_", "SDL_"))}
        observed["mode"] = mode
        artifacts.append(inspect_run(directory, observed, kind, mode, instructions, interval, checkpoints,
                                     min_blocks, fastmem, linking, restore_at if label == "right" else 0))
        result["runs"].append(observed)
    if all(run["passed"] for run in result["runs"]):
        if kind == "state" and result["runs"][0]["checkpoints"] != result["runs"][1]["checkpoints"]:
            result["differences"].append({"field": "checkpoint frame/ticks"})
        for left, right in zip(*artifacts):
            offset = first_difference(left, right)
            if offset is not None:
                result["differences"].append({"artifact": left.name, "first_different_byte": offset})
    result["inputs_unchanged"] = all(sha256(path) == hashes[str(path)] for path in paths)
    result["passed"] = (all(run["passed"] for run in result["runs"])
                        and not result["differences"] and result["inputs_unchanged"])
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("media", type=Path)
    parser.add_argument("--bios", type=Path, required=True)
    parser.add_argument("--binary", type=Path, default=ROOT / "build/cupid-ps1")
    parser.add_argument("--kind", choices=tuple(COVERAGE), default="trace")
    parser.add_argument("--left", choices=("interp", "cached", "recomp"), default="interp")
    parser.add_argument("--right", choices=("interp", "cached", "recomp"), default="cached")
    parser.add_argument("--instructions", type=int, default=50000)
    parser.add_argument("--interval", type=int, default=60)
    parser.add_argument("--checkpoints", type=int, default=5)
    parser.add_argument("--restore-at", type=int, default=0,
                        help="reload this checkpoint in the right process, then compare its continuation")
    parser.add_argument("--minimum-blocks", type=int, default=100)
    parser.add_argument("--fastmem", choices=("off", "lut", "mmap"), default="off")
    parser.add_argument("--no-linking", action="store_true")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--revision")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--hash-input", type=Path, action="append", default=[])
    args = parser.parse_args()
    if os.name != "posix":
        parser.error("run in the supported Linux build environment")
    output = args.output or ROOT / ".local-research" / ("compare-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    try:
        result = run_comparison(args.binary, args.bios, args.media, output, kind=args.kind,
                                modes=(args.left, args.right), instructions=args.instructions,
                                interval=args.interval, checkpoints=args.checkpoints,
                                min_blocks=args.minimum_blocks, fastmem=args.fastmem, linking=not args.no_linking,
                                timeout=args.timeout, revision=args.revision, hash_inputs=args.hash_input,
                                restore_at=args.restore_at)
    except (OSError, ValueError) as error:
        parser.exit(2, f"comparison failed: {error}\n")
    print(f"{'PASS' if result['passed'] else 'FAIL'}: {output.resolve() / 'result.json'}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
