#!/usr/bin/env python3
"""Export a pinned Spack package closure as spack-export/1 JSON."""

from __future__ import annotations

import argparse
import ast
import inspect
import json
import subprocess
from pathlib import Path
from typing import Any

import spack.repo
import spack.paths
import spack.deptypes
from spack.spec import Spec


def text(value: Any) -> str:
    return str(value)


def digest_text(value: Any) -> str | None:
    if isinstance(value, dict):
        selected = value.get("sha256")
        return text(selected) if selected else None
    for attribute in ("digest", "sha256"):
        selected = getattr(value, attribute, None)
        if selected:
            rendered = text(selected)
            if rendered.startswith("sha256:"):
                rendered = rendered.removeprefix("sha256:")
            if len(rendered) == 64:
                return rendered
    return None


def static_return(function: Any) -> tuple[list[str] | None, str | None]:
    if function is None:
        return [], None
    try:
        source = inspect.getsource(function)
        module = ast.parse(inspect.cleandoc(source))
    except (OSError, TypeError, SyntaxError) as error:
        return None, f"source is unavailable: {error}"
    definitions = [node for node in ast.walk(module) if isinstance(node, ast.FunctionDef)]
    if len(definitions) != 1:
        return None, "method is not one statically inspectable function"
    statements = definitions[0].body
    if len(statements) != 1 or not isinstance(statements[0], ast.Return):
        return None, "method executes Python instead of returning one literal list"
    try:
        value = ast.literal_eval(statements[0].value)
    except (ValueError, TypeError):
        return None, "return value is computed by Python at build time"
    if not isinstance(value, (list, tuple)) or not all(isinstance(item, str) for item in value):
        return None, "return value is not a literal string list"
    return list(value), None


def package_versions(package: Any, losses: list[dict[str, str]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for version, metadata in sorted(package.versions.items(), key=lambda item: item[0]):
        record: dict[str, Any] = {"version": text(version)}
        checksum = digest_text(metadata)
        try:
            url = package.url_for_version(version)
        except Exception as error:  # Spack packages deliberately use arbitrary Python here.
            url = None
            losses.append(
                {
                    "kind": "dynamic_value",
                    "field": f"versions.{version}.url",
                    "reason": f"url_for_version executes Python: {error}",
                }
            )
        if url:
            record["url"] = text(url)
        if checksum:
            record["sha256"] = checksum
        if url and not checksum:
            losses.append(
                {
                    "kind": "unsupported_value",
                    "field": f"versions.{version}.sha256",
                    "reason": "Spack version has no SHA-256 digest",
                }
            )
        output.append(record)
    return output


def package_variants(package: Any, losses: list[dict[str, str]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for when, definitions in sorted(package.variants.items(), key=lambda item: text(item[0])):
        for name, definition in sorted(definitions.items()):
            values = getattr(definition, "values", ())
            if values is None or callable(values):
                losses.append(
                    {
                        "kind": "dynamic_value",
                        "field": f"variants.{name}.values",
                        "reason": "variant values are computed by Python",
                    }
                )
                continue
            raw_values = list(values)
            scalar_values = [
                value for value in raw_values if isinstance(value, (str, bool, int, float))
            ]
            if len(scalar_values) != len(raw_values):
                losses.append(
                    {
                        "kind": "unsupported_value",
                        "field": f"variants.{name}.values",
                        "reason": "conditional or structured variant values require Python",
                    }
                )
                continue
            output.append(
                {
                    "name": name,
                    "values": scalar_values,
                    "default": getattr(definition, "default", False),
                    "when": text(when) if text(when) else None,
                }
            )
    return output


def package_dependencies(package: Any) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for when, dependencies in sorted(package.dependencies.items(), key=lambda item: text(item[0])):
        for name, dependency in sorted(dependencies.items()):
            dependency_spec = getattr(dependency, "spec", Spec(name))
            version = text(dependency_spec.versions)
            if version == ":":
                version = None
            types = sorted(spack.deptypes.flag_to_tuple(dependency.depflag))
            output.append(
                {
                    "name": name,
                    "constraint": version,
                    "when": text(when) if text(when) != "" else None,
                    "types": types,
                    "virtual": spack.repo.PATH.is_virtual(name),
                }
            )
    return output


def package_provides(package: Any, losses: list[dict[str, str]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for when, provided_specs in sorted(package.provided.items(), key=lambda item: text(item[0])):
        package_condition = text(when)
        if package_condition not in ("", package.name):
            losses.append(
                {
                    "kind": "unsupported_value",
                    "field": "provides.when",
                    "reason": f"conditional provide requires {package_condition}",
                }
            )
        for provided in sorted(provided_specs, key=text):
            version = text(getattr(provided, "versions", ":"))
            output.append(
                {
                    "name": getattr(provided, "name", text(provided)),
                    "version": None if version == ":" else version,
                }
            )
    return output


def package_conflicts(package: Any) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for when, entries in sorted(package.conflicts.items(), key=lambda item: text(item[0])):
        for constraint, message in entries:
            output.append(
                {
                    "constraint": text(constraint),
                    "when": text(when) if when else None,
                    "message": message or "",
                }
            )
    return output


def package_patches(package: Any, losses: list[dict[str, str]]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for when, patches in sorted(package.patches.items(), key=lambda item: text(item[0])):
        for patch in patches:
            url = getattr(patch, "url", None)
            checksum = digest_text(patch)
            if not url and not checksum:
                losses.append(
                    {
                        "kind": "dynamic_value",
                        "field": "patches",
                        "reason": "local or generated patch has no exportable URL or SHA-256",
                    }
                )
                continue
            output.append(
                {
                    "url": text(url) if url else None,
                    "sha256": checksum,
                    "when": text(when) if when else None,
                }
            )
    return output


def package_build(package_class: type, losses: list[dict[str, str]]) -> dict[str, Any] | None:
    names = [base.__name__ for base in package_class.__mro__]
    if any("CMakePackage" in name for name in names):
        system = "cmake"
        method_name = "cmake_args"
    elif any("AutotoolsPackage" in name for name in names):
        system = "autotools"
        method_name = "configure_args"
    elif any("MakefilePackage" in name for name in names):
        system = "makefile"
        method_name = None
    else:
        losses.append(
            {
                "kind": "unknown_build_system",
                "field": "build.system",
                "reason": names[1] if len(names) > 1 else package_class.__name__,
            }
        )
        return None
    if method_name is None:
        arguments, dynamic = [], None
    else:
        arguments, dynamic = static_return(getattr(package_class, method_name, None))
    if dynamic:
        losses.append(
            {
                "kind": "dynamic_value",
                "field": "build.arguments",
                "reason": dynamic,
            }
        )
        arguments = []
    phases = tuple(getattr(package_class, "phases", ()))
    known = {
        "cmake": ("cmake", "build", "install"),
        "autotools": ("autoreconf", "configure", "build", "install"),
        "makefile": ("edit", "build", "install"),
    }[system]
    custom = [phase for phase in phases if phase not in known]
    if custom:
        losses.append(
            {
                "kind": "custom_phase",
                "field": "build.phases",
                "reason": ", ".join(custom),
            }
        )
    return {"system": system, "arguments": arguments}


def export_package(name: str) -> dict[str, Any]:
    package_class = spack.repo.PATH.get_pkg_class(name)
    package = package_class(Spec(name))
    losses: list[dict[str, str]] = []
    return {
        "name": name,
        "versions": package_versions(package, losses),
        "variants": package_variants(package, losses),
        "dependencies": package_dependencies(package),
        "provides": package_provides(package, losses),
        "conflicts": package_conflicts(package),
        "patches": package_patches(package, losses),
        "build": package_build(package_class, losses),
        "losses": losses,
    }


def closure(roots: list[str]) -> list[str]:
    names: set[str] = set()
    pending = [Spec(root).name for root in roots]
    while pending:
        selected = pending.pop()
        name = selected.name if isinstance(selected, Spec) else selected
        if not name or name in names:
            continue
        if spack.repo.PATH.is_virtual(name):
            providers = spack.repo.PATH.providers_for(name)
            pending.extend(spec.name for spec in providers)
            continue
        names.add(name)
        package_class = spack.repo.PATH.get_pkg_class(name)
        package = package_class(Spec(name))
        for dependencies in package.dependencies.values():
            pending.extend(dependencies)
    return sorted(names)


def revision() -> str:
    process = subprocess.run(
        ["git", "-C", str(spack.paths.prefix), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    )
    return process.stdout.strip()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", action="append", required=True)
    parser.add_argument("--spack-checkout-digest", required=True)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    names = closure(arguments.root)
    document = {
        "schema": "spack-export/1",
        "spack": {
            "revision": revision(),
            "checkout_digest": arguments.spack_checkout_digest,
        },
        "provenance": {
            "profile": "slsa_provenance/1",
            "carrier": "liquid wrapped action",
        },
        "roots": arguments.root,
        "packages": [export_package(name) for name in names],
    }
    arguments.output.write_text(
        json.dumps(document, default=text, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
