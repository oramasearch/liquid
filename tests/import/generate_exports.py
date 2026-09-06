#!/usr/bin/env python3
"""Regenerate deterministic Spack interchange fixtures used without a Spack checkout."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parent / "fixtures"
LEAVES = [
    "zlib",
    "bzip2",
    "xz",
    "expat",
    "libffi",
    "pcre2",
    "libpng",
    "sqlite",
    "ncurses",
    "readline",
    "cmake",
    "openssl",
    "openmpi",
    "hdf5",
    "netcdf-c",
    "openblas",
    "fftw",
    "python",
    "boost",
    "pkgconf",
]
ROOTS = ["hdf5+mpi", "netcdf-c", "openblas", "fftw", "python", "cmake", "boost", "openssl"]
ROOT_NAMES = {"hdf5", "netcdf-c", "openblas", "fftw", "python", "cmake", "boost", "openssl"}


def checksum(value: str) -> str:
    return hashlib.sha256(value.encode()).hexdigest()


def dependency(
    name: str,
    constraint: str | None = None,
    when: str | None = None,
    types: list[str] | None = None,
) -> dict[str, Any]:
    return {
        "name": name,
        "constraint": constraint,
        "when": when,
        "types": types or ["link"],
        "virtual": name == "mpi",
    }


def package(name: str, index: int) -> dict[str, Any]:
    version = f"1.{index}.0"
    build_system = "cmake" if index % 2 == 0 else "autotools"
    return {
        "name": name,
        "versions": [
            {
                "version": version,
                "url": f"https://example.invalid/{name}-{version}.tar.gz",
                "sha256": checksum(f"{name}-{version}"),
            }
        ],
        "variants": [],
        "dependencies": [],
        "provides": [],
        "conflicts": [],
        "patches": [],
        "build": {"system": build_system, "arguments": []},
        "losses": [],
    }


def twenty_packages() -> list[dict[str, Any]]:
    output = [package(name, index + 1) for index, name in enumerate(LEAVES)]
    values = {item["name"]: item for item in output}
    values["libpng"]["dependencies"] = [dependency("zlib", "1:")]
    values["readline"]["dependencies"] = [dependency("ncurses")]
    values["hdf5"]["variants"] = [
        {"name": "mpi", "values": [True, False], "default": True, "when": None}
    ]
    values["hdf5"]["dependencies"] = [
        dependency("zlib"),
        dependency("mpi", "3:", "+mpi", ["build", "link", "run"]),
    ]
    values["hdf5"]["conflicts"] = [
        {"constraint": "~mpi", "when": None, "message": "MPI-disabled HDF5 is excluded"}
    ]
    values["hdf5"]["losses"] = [
        {
            "kind": "dynamic_value",
            "field": "build.arguments",
            "reason": "Fortran arguments depend on the selected compiler",
        },
        {
            "kind": "custom_phase",
            "field": "build.phases",
            "reason": "cache_test_examples",
        },
    ]
    values["netcdf-c"]["dependencies"] = [dependency("hdf5"), dependency("zlib")]
    values["python"]["dependencies"] = [
        dependency("openssl"),
        dependency("sqlite"),
        dependency("libffi"),
    ]
    values["boost"]["dependencies"] = [dependency("zlib"), dependency("bzip2")]
    values["openmpi"]["provides"] = [{"name": "mpi", "version": "3:5"}]
    values["zlib"]["patches"] = [
        {
            "url": "https://example.invalid/zlib-relocation.patch",
            "sha256": None,
            "when": None,
        }
    ]
    values["pkgconf"]["build"] = {"system": "python", "arguments": []}
    return output


def document(packages: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schema": "spack-export/1",
        "spack": {
            "revision": "0123456789abcdef0123456789abcdef01234567",
            "checkout_digest": f"sha256:{checksum('spack-checkout')}",
        },
        "provenance": {
            "profile": "slsa_provenance/1",
            "carrier": "liquid wrapped action",
        },
        "roots": ROOTS,
        "packages": packages,
    }


def five_hundred_packages() -> list[dict[str, Any]]:
    output = twenty_packages()
    generated = [package(f"spack-fixture-{index:03d}", index + 21) for index in range(480)]
    for index, item in enumerate(generated):
        if index:
            item["dependencies"] = [dependency(generated[index - 1]["name"])]
        if index % 50 == 0:
            item["losses"] = [
                {
                    "kind": "dynamic_value",
                    "field": "build.arguments",
                    "reason": "fixture models a package with computed flags",
                }
            ]
    by_name = {item["name"]: item for item in output}
    by_name["hdf5"]["dependencies"].append(dependency(generated[-1]["name"]))
    for name in LEAVES:
        if name not in ROOT_NAMES | {"openmpi", "zlib"}:
            by_name["hdf5"]["dependencies"].append(dependency(name))
    return output + generated


def write(name: str, packages: list[dict[str, Any]]) -> None:
    path = ROOT / name
    path.write_text(json.dumps(document(packages), indent=2, sort_keys=True) + "\n")


def main() -> int:
    ROOT.mkdir(parents=True, exist_ok=True)
    write("spack-20.json", twenty_packages())
    write("spack-500.json", five_hundred_packages())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
