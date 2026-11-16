#!/usr/bin/env python3
# SPDX-License-Identifier: MIT

"""Maintain liquid's canonical tree-sitter grammar lock and documentation table."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import shutil
import tarfile
import tempfile
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[2]
LOCK = ROOT / "modules/frontends/grammars/grammars.lock"
DOC = ROOT / "docs/library/grammars.md"
BEGIN = "<!-- BEGIN GENERATED GRAMMARS -->"
END = "<!-- END GENERATED GRAMMARS -->"

LANGUAGE_ALIASES = {
    "c++": "cpp",
    "c#": "c_sharp",
    "c-sharp": "c_sharp",
    "objective-c": "objc",
    "protocol-buffers": "proto",
}

DEFAULT_EXTENSIONS = {
    "bash": [".bash", ".sh"],
    "c": [".c", ".h"],
    "c#": [".cs"],
    "c++": [".cc", ".cpp", ".cxx", ".hh", ".hpp", ".hxx"],
    "cmake": [".cmake", "CMakeLists.txt"],
    "css": [".css"],
    "dart": [".dart"],
    "dockerfile": ["Dockerfile"],
    "elixir": [".ex", ".exs"],
    "elm": [".elm"],
    "erlang": [".erl", ".hrl"],
    "fortran": [".f", ".f90", ".f95", ".f03", ".f08"],
    "go": [".go"],
    "haskell": [".hs"],
    "hcl": [".hcl", ".tf"],
    "html": [".html", ".htm"],
    "java": [".java"],
    "javascript": [".js", ".jsx", ".mjs", ".cjs"],
    "json": [".json"],
    "julia": [".jl"],
    "kotlin": [".kt", ".kts"],
    "latex": [".tex"],
    "lua": [".lua"],
    "make": ["Makefile", ".mk"],
    "markdown": [".md", ".markdown"],
    "nix": [".nix"],
    "objective-c": [".m", ".mm"],
    "ocaml": [".ml", ".mli"],
    "perl": [".pl", ".pm"],
    "php": [".php"],
    "protocol-buffers": [".proto"],
    "python": [".py"],
    "r": [".r", ".R"],
    "ruby": [".rb"],
    "rust": [".rs"],
    "scala": [".scala", ".sc"],
    "scss": [".scss"],
    "solidity": [".sol"],
    "sql": [".sql"],
    "svelte": [".svelte"],
    "swift": [".swift"],
    "toml": [".toml"],
    "tsx": [".tsx"],
    "typescript": [".ts"],
    "verilog": [".v", ".vh", ".sv", ".svh"],
    "vue": [".vue"],
    "xml": [".xml", ".xsd", ".xsl", ".xslt"],
    "yaml": [".yaml", ".yml"],
    "zig": [".zig"],
}


def sha256_bytes(value: bytes) -> str:
    return hashlib.sha256(value).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def normalise_repository(repository: str) -> str:
    value = repository.removesuffix(".git").rstrip("/")
    if "/" in value and "://" not in value and not value.startswith("git@"):
        value = "https://github.com/" + value
    if value.startswith("git@github.com:"):
        value = "https://github.com/" + value.removeprefix("git@github.com:")
    if not value.startswith("https://github.com/"):
        raise ValueError("repository must be an https://github.com URL or owner/name")
    return value


def archive_url(repository: str, commit: str) -> str:
    return f"{repository}/archive/{commit}.tar.gz"


def download(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "liquid-grammar-lock/1"})
    with urllib.request.urlopen(request) as response:
        return response.read()


def safe_extract(archive: bytes, destination: Path) -> Path:
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as source:
        destination_resolved = destination.resolve()
        for member in source.getmembers():
            target = (destination / member.name).resolve()
            if destination_resolved not in target.parents and target != destination_resolved:
                raise ValueError(f"archive member escapes extraction root: {member.name}")
            if member.issym() or member.islnk():
                raise ValueError(f"archive contains a link: {member.name}")
        source.extractall(destination)
    roots = [path for path in destination.iterdir() if path.is_dir()]
    if len(roots) != 1:
        raise ValueError("grammar archive must contain exactly one root directory")
    return roots[0]


def read_json(path: Path) -> dict | None:
    if not path.exists():
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError:
        return None


def grammar_configuration(root: Path, language: str) -> tuple[dict | None, Path]:
    configuration = read_json(root / "tree-sitter.json")
    if not configuration:
        return None, root
    names = {language, LANGUAGE_ALIASES.get(language, language)}
    for grammar in configuration.get("grammars", []):
        if grammar.get("name") in names:
            return grammar, root / grammar.get("path", ".")
    grammars = configuration.get("grammars", [])
    if len(grammars) == 1:
        grammar = grammars[0]
        return grammar, root / grammar.get("path", ".")
    raise ValueError(f"tree-sitter.json has no grammar named {language}")


def parser_path(root: Path, grammar_root: Path) -> Path:
    direct = grammar_root / "src/parser.c"
    if direct.exists():
        return direct
    candidates = sorted(root.glob("**/src/parser.c"))
    if len(candidates) != 1:
        raise ValueError("cannot select one generated parser.c")
    return candidates[0]


def query_path(root: Path, grammar_root: Path, grammar: dict | None) -> Path | None:
    candidates: list[Path] = []
    if grammar and grammar.get("tags"):
        values = grammar["tags"]
        if isinstance(values, str):
            values = [values]
        for value in values:
            candidates.extend([grammar_root / value, root / value])
    candidates.extend([grammar_root / "queries/tags.scm", root / "queries/tags.scm"])
    for candidate in candidates:
        if candidate.exists() and candidate.is_file():
            return candidate
    return None


def extensions_for(language: str, grammar: dict | None) -> list[str]:
    values: list[str] = []
    if grammar:
        for value in grammar.get("file-types") or []:
            if value in {"CMakeLists.txt", "Dockerfile", "Makefile"}:
                values.append(value)
            elif value.startswith("."):
                values.append(value)
            else:
                values.append("." + value)
    if not values:
        values = DEFAULT_EXTENSIONS.get(language, [])
    if not values:
        raise ValueError(f"no extensions are known for {language}")
    return sorted(set(values), key=lambda value: (value.lower(), value))


def license_for(root: Path, grammar: dict | None) -> str:
    configuration = read_json(root / "tree-sitter.json") or {}
    metadata = configuration.get("metadata", {})
    if metadata.get("license"):
        return metadata["license"]
    package = read_json(root / "package.json") or {}
    if package.get("license"):
        return package["license"]
    cargo = root / "Cargo.toml"
    if cargo.exists():
        match = re.search(r'^license\s*=\s*"([^"]+)"', cargo.read_text(), re.MULTILINE)
        if match:
            return match.group(1)
    raise ValueError("grammar does not declare a machine-readable licence")


def relative(root: Path, path: Path) -> str:
    return path.relative_to(root).as_posix()


def inspect_archive(language: str, repository: str, commit: str, archive: bytes) -> dict:
    work_root = ROOT / ".grammar-work"
    work_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work_root) as temporary:
        extracted = safe_extract(archive, Path(temporary))
        grammar, grammar_root = grammar_configuration(extracted, language)
        parser = parser_path(extracted, grammar_root)
        text = parser.read_text(errors="replace")
        version_match = re.search(r"^#define LANGUAGE_VERSION (\d+)$", text, re.MULTILINE)
        symbol_match = re.search(
            r"\bconst\s+TSLanguage\s*\*\s*(tree_sitter_[A-Za-z0-9_]+)\s*\(void\)\s*\{",
            text,
        )
        if not version_match or not symbol_match:
            raise ValueError("parser.c does not declare its ABI version and language symbol")
        scanners = []
        for name in ("scanner.c", "scanner.cc", "scanner.cpp"):
            scanner = parser.parent / name
            if scanner.exists():
                scanners.append(
                    {
                        "path": relative(extracted, scanner),
                        "sha256": sha256_file(scanner),
                    }
                )
        tags = query_path(extracted, grammar_root, grammar)
        return {
            "archive_sha256": sha256_bytes(archive),
            "commit": commit,
            "extensions": extensions_for(language, grammar),
            "language": language,
            "license": license_for(extracted, grammar),
            "parser": {
                "path": relative(extracted, parser),
                "sha256": sha256_file(parser),
            },
            "repository": repository,
            "scanners": scanners,
            "symbol": symbol_match.group(1),
            "tags": (
                {"path": relative(extracted, tags), "sha256": sha256_file(tags)}
                if tags
                else None
            ),
            "tree_sitter_abi": int(version_match.group(1)),
        }


def load_lock() -> dict:
    value = json.loads(LOCK.read_text())
    if value.get("version") != 1 or not isinstance(value.get("grammars"), list):
        raise ValueError("unsupported grammar lock format")
    return value


def write_lock(value: dict) -> None:
    value["grammars"] = sorted(value["grammars"], key=lambda item: item["language"])
    LOCK.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def markdown_table(grammars: list[dict]) -> str:
    lines = [
        "| Language | Extensions | Upstream commit | ABI | Tags | Licence |",
        "|---|---|---|---:|---|---|",
    ]
    for grammar in grammars:
        repository = grammar["repository"]
        commit = grammar["commit"]
        upstream = f"[`{commit[:12]}`]({repository}/commit/{commit})"
        extensions = ", ".join(f"`{value}`" for value in grammar["extensions"])
        tags = "pinned" if grammar["tags"] else "unsupported"
        lines.append(
            f"| {grammar['language']} | {extensions} | {upstream} | "
            f"{grammar['tree_sitter_abi']} | {tags} | {grammar['license']} |"
        )
    return "\n".join(lines)


def update_documentation(value: dict) -> None:
    if not DOC.exists():
        return
    text = DOC.read_text()
    if BEGIN not in text or END not in text:
        raise ValueError(f"{DOC} is missing generated table markers")
    before, rest = text.split(BEGIN, 1)
    _, after = rest.split(END, 1)
    generated = markdown_table(value["grammars"])
    DOC.write_text(f"{before}{BEGIN}\n{generated}\n{END}{after}")


def add(language: str, repository: str, commit: str) -> None:
    repository = normalise_repository(repository)
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("commit must be a full lowercase 40-character SHA-1")
    archive = download(archive_url(repository, commit))
    entry = inspect_archive(language, repository, commit, archive)
    value = load_lock()
    value["grammars"] = [
        grammar for grammar in value["grammars"] if grammar["language"] != language
    ]
    value["grammars"].append(entry)
    write_lock(value)
    update_documentation(value)
    shutil.rmtree(ROOT / ".grammar-work", ignore_errors=True)
    print(
        f"locked {language} at {commit}: archive sha256:{entry['archive_sha256']} "
        f"parser sha256:{entry['parser']['sha256']}"
    )


def generate_docs() -> None:
    update_documentation(load_lock())


def remove(language: str) -> None:
    value = load_lock()
    remaining = [
        grammar for grammar in value["grammars"] if grammar["language"] != language
    ]
    if len(remaining) == len(value["grammars"]):
        raise ValueError(f"grammar is not locked: {language}")
    value["grammars"] = remaining
    write_lock(value)
    update_documentation(value)
    print(f"removed {language} from the grammar lock")


def main() -> int:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    add_parser = subparsers.add_parser("add")
    add_parser.add_argument("language")
    add_parser.add_argument("repository")
    add_parser.add_argument("commit")
    remove_parser = subparsers.add_parser("remove")
    remove_parser.add_argument("language")
    subparsers.add_parser("generate-docs")
    arguments = parser.parse_args()
    try:
        if arguments.command == "add":
            add(arguments.language, arguments.repository, arguments.commit)
        elif arguments.command == "remove":
            remove(arguments.language)
        else:
            generate_docs()
    except (OSError, ValueError, urllib.error.URLError) as failure:
        parser.exit(1, f"error: {failure}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
