#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

import argparse
import os
import pathlib
import re
import shutil
import socket
import subprocess
import sys


BLOCK = re.compile(
    r"<!-- guides-run: ([a-z0-9-]+) -->\s*```sh\n(.*?)\n```",
    re.DOTALL,
)
SHELL_FENCE = re.compile(r"```sh\n")


def available_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--liquid", required=True, type=pathlib.Path)
    parser.add_argument("--guides", required=True, type=pathlib.Path)
    parser.add_argument("--fixtures", required=True, type=pathlib.Path)
    parser.add_argument("--work", required=True, type=pathlib.Path)
    parser.add_argument("--guide", help="Run one guide that needs no FUSE mount")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    if arguments.guide is None and not pathlib.Path("/dev/fuse").exists():
        print("guides-run skipped: /dev/fuse is unavailable")
        return 77
    shutil.rmtree(arguments.work, ignore_errors=True)
    arguments.work.mkdir(parents=True)
    found = 0
    guides = (
        [arguments.guides / arguments.guide]
        if arguments.guide
        else sorted(arguments.guides.glob("*.md"))
    )
    for guide in guides:
        content = guide.read_text()
        blocks = list(BLOCK.finditer(content))
        if len(blocks) != len(SHELL_FENCE.findall(content)):
            print(f"{guide}: every sh block must have a guides-run marker", file=sys.stderr)
            return 1
        for match in blocks:
            found += 1
            name = match.group(1)
            script = match.group(2)
            directory = arguments.work / name
            directory.mkdir()
            environment = os.environ.copy()
            environment["PATH"] = f"{arguments.liquid.parent}:{environment['PATH']}"
            environment["HOME"] = str(directory / "home")
            environment["LIQUID_GUIDE_FIXTURES"] = str(arguments.fixtures)
            if "LIQUID_GUIDE_PORT" in script:
                environment["LIQUID_GUIDE_PORT"] = str(available_port())
            environment["LIQUID_WRAP_CACHE"] = str(directory / "wrap-cache")
            environment["TMPDIR"] = str(directory / "tmp")
            pathlib.Path(environment["HOME"]).mkdir()
            pathlib.Path(environment["TMPDIR"]).mkdir()
            result = subprocess.run(
                ["bash", "-eu", "-o", "pipefail", "-c", script],
                cwd=directory,
                env=environment,
                text=True,
            )
            if result.returncode != 0:
                print(f"{guide}: guides-run block {name} failed", file=sys.stderr)
                return result.returncode
    if found == 0:
        print("guides-run found no marked shell blocks", file=sys.stderr)
        return 1
    print(f"guides-run executed {found} marked shell blocks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
