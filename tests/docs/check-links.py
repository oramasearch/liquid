#!/usr/bin/env python3

import argparse
import pathlib
import re


LINK = re.compile(r"\[[^\]]*\]\(([^)#][^)]*)\)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=pathlib.Path)
    arguments = parser.parse_args()
    files = sorted((arguments.root / "docs").glob("**/*.md"))
    files.append(arguments.root / "README.md")
    broken: list[str] = []
    for source in files:
        for match in LINK.finditer(source.read_text()):
            target = match.group(1).split("#", 1)[0]
            if not target or target.startswith(("http", "mailto:")):
                continue
            path = (source.parent / target).resolve()
            if not path.exists() and not pathlib.Path(f"{path}.md").exists():
                broken.append(f"{source.relative_to(arguments.root)} -> {target}")
    print(f"broken links: {len(broken)}")
    for value in broken:
        print(f"  {value}")
    return 1 if broken else 0


if __name__ == "__main__":
    raise SystemExit(main())
