#!/usr/bin/env python3
"""Build the pinned audio C interface without replacing the bundled runtime."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

REVISION = "e83424d5928ab8513d2d082779c275765dee31b9"
ARCHIVE_SHA256 = "06c00f37d28e5984944951c20d0845c7931e691c59820922230915d205131a47"
URL = f"https://codeberg.org/soundtouch/soundtouch/archive/{REVISION}.tar.gz"
ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build/dependencies/soundtouch-2.3.3")
    parser.add_argument("--archive", type=Path, help="Use a cached archive with the pinned SHA-256")
    parser.add_argument("--compiler", type=Path, help="C++ compiler; its directory is added to the child PATH")
    parser.add_argument("--generator", default="Ninja")
    parser.add_argument("--jobs", type=int, default=2)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    archive = output / "source.tar.gz"
    if args.archive:
        archive.write_bytes(args.archive.read_bytes())
    else:
        with urllib.request.urlopen(URL, timeout=60) as response:
            archive.write_bytes(response.read())
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != ARCHIVE_SHA256:
        raise ValueError(f"Source archive SHA-256 mismatch: {digest}")
    with tarfile.open(archive) as source:
        source.extractall(output / "source", filter="data")
    source_dir = output / "source/soundtouch"
    # The resource uses only Windows SDK definitions; MFC is not required.
    resource = source_dir / "source/SoundTouchDLL/SoundTouchDLL.rc"
    resource_bytes = resource.read_bytes()
    if resource_bytes.count(b"afxres.h") != 2:
        raise ValueError("Unexpected resource include in pinned source")
    resource.write_bytes(resource_bytes.replace(b"afxres.h", b"winres.h"))
    env = dict(os.environ)
    options = ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_POLICY_VERSION_MINIMUM=3.5",
               "-DBUILD_SHARED_LIBS=OFF", "-DSOUNDTOUCH_DLL=ON", "-DSOUNDSTRETCH=OFF",
               "-DINTEGER_SAMPLES=OFF", "-DOPENMP=OFF"]
    if args.compiler:
        compiler = args.compiler.resolve(strict=True)
        env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
        options.append(f"-DCMAKE_CXX_COMPILER={compiler}")
    commands = [["cmake", "-S", str(source_dir), "-B", str(output / "build"),
                 "-G", args.generator, *options],
                ["cmake", "--build", str(output / "build"), "--config", "Release",
                 "--parallel", str(args.jobs)]]
    for command in commands:
        subprocess.run(command, env=env, check=True)
    receipt = {"version": "2.3.3", "revision": REVISION, "archive_url": URL,
               "archive_sha256": digest, "commands": commands,
               "source_patches": [{"file": "source/SoundTouchDLL/SoundTouchDLL.rc",
                                   "change": "Use Windows SDK winres.h instead of MFC afxres.h"}],
               "scope": "Dependency build only; does not qualify the emulator or the bundled ELF binary."}
    (output / "build-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(f"Build receipt: {output / 'build-receipt.json'}")


if __name__ == "__main__":
    main()
