# Relocatable install trees

Issue #73 chooses position-independent artifacts by construction. Liquid does not rewrite a built
binary and does not require an installed artifact to remain at a content-addressed store path. The
fixed build root, `/liquid/work/<action-id>/`, is a sandbox address and is nobody's install prefix.
An install tree therefore carries only references relative to itself.

The canonical output layout is an unrooted tree with `bin/`, `lib/`, `include/`, and `share/` below
its root. Materialising that tree at any prefix produces
`prefix/{bin,lib,include,share}`. Copying it to another prefix must not require changing its bytes.

## Rules by artifact kind

| Artifact | Rule liquid applies or checks |
|---|---|
| ELF | Install-mode linker defaults are `-Wl,-rpath,$ORIGIN/../lib -Wl,-z,origin -Wl,--enable-new-dtags`. Dynamic dependencies may not contain an absolute `DT_NEEDED`, and `DT_RPATH` or `DT_RUNPATH` entries may not be absolute. |
| Mach-O | Shared libraries use `@rpath` install names. Executables and libraries receive `-Wl,-rpath,@loader_path/../lib`; `LC_ID_DYLIB` and `LC_RPATH` may not be absolute. |
| PE | Dependent DLLs live beside the executable. There is no embedded lookup path. |
| Scripts | Use an interpreter such as `#!/usr/bin/env sh` and derive the tree from `$(dirname "$0")`; do not embed the build or install prefix. |
| pkg-config | Set `prefix=${pcfiledir}/../..` and derive the other directories from `prefix`. Absolute paths in assignments or flags are rejected. |
| CMake packages | Use `configure_package_config_file` and paths derived from `PACKAGE_PREFIX_DIR` or `CMAKE_CURRENT_LIST_DIR`. Absolute paths in installed `.cmake` files are rejected. |
| Data | Locate the executable through `/proc/self/exe` on Linux or `_NSGetExecutablePath` on macOS, canonicalise its directory, and resolve `../share/<project>` from it. Libraries that do not control an executable accept a resource root from their caller. |

The ELF defaults produce `DT_RUNPATH`, not legacy `DT_RPATH`. `$ORIGIN` expansion is explicitly
enabled. Mach-O system dependencies may retain platform paths such as `/usr/lib`; the project's
own install name and loader search path remain relative.

## Build-system conventions

`liquid wrap --install-prefix <absolute-path-in-sandbox>` enables install mode. The logical prefix
is passed as `CMAKE_INSTALL_PREFIX`, while `DESTDIR=/liquid/work/out/prefix` stages files inside the
declared output area. The published output-tree root is the logical prefix stripped from that
staging path, so consumers receive `bin`, `lib`, `include`, and `share`, not the sandbox directories.

For CMake configure commands, `wrap` supplies the logical prefix, the relative install RPATH, and,
on macOS, the `@rpath` install-name directory unless the command already supplies those values.
`cmake --install` observes `DESTDIR`. Projects should generate package configuration with
`configure_package_config_file` and install exports normally.

For a directly invoked Autotools `configure`, `wrap` adds `--prefix=<logical-prefix>` unless it is
already present. `make install` observes `DESTDIR`. A compound shell command must pass the same
`--prefix` itself. Projects must avoid `-rpath` values based on the logical prefix and should use
the install-mode linker flags from `LDFLAGS`. Plain Make projects read `DESTDIR`,
`CMAKE_INSTALL_PREFIX` when useful, or `LIQUID_INSTALL_TREE=out/prefix`; their installed metadata
still follows the table above.

## Enforcement and API

`build::relocation_check(output_tree, options)` performs CPU scanning through `runtime::offload`
and returns a `relocation_report`. Truncated linked-binary symbol inspection adds a finding and
blocks relocation approval, preserving the other collected findings (liquid #475).
Every finding contains the relative file, byte offset, and a
short context. The scanner finds:

- the virtual build-root string and an optional declared install-prefix string in any regular file;
- absolute ELF runtime paths and dependency names;
- absolute Mach-O install names and runtime paths;
- absolute pkg-config and CMake package paths; and
- absolute script interpreters.

Binary path checks use the ELF and Mach-O readers described in [abi.md](abi.md). Install-mode
`wrap` publication runs the same check on the staged install tree before storing its canonical
output tree. A finding fails publication with `errc::invalid_argument`; the error names the first
three findings. `--allow-embedded-prefix` permits publication and appends the waiver and finding
count to the result's recorded scheduling reason, which `liquid why` displays. The option is valid
only with `--install-prefix`.

The public check is also the integration point for `liquid project`. That command and package
projection remain issue #28; they must call the check before accepting an install tree.

## Verification

The CMake fixture installs a shared library, executable, header, pkg-config file, and CMake package
configuration. Its install tree is materialised at one host path, moved to another, and executed
without changing the artifacts. Portable tests cover clean and deliberately embedded prefixes,
absolute package metadata, and an absolute Mach-O install name. The mount-backed wrapper case also
checks rejection and the recorded waiver.
