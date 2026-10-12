# liquid

liquid helps you keep file history and build software in one tool. Import a folder, capture
edits as signed **drips**, and give a group of drips a reviewable name as a **change**. The same
tool can reuse build results and package the outputs. Start below by recording a file and
reading its named history.

**Status:** The current release is **v0.1.0** (2026-10-12), the first tagged release; its
[release notes](docs/releases/v0.1.0.md) list everything in it. The local repository workflow
below is implemented in the C++26 library, **libliquid**, and its `liquid` command-line client.
The authoritative [implementation status](docs/library/status.md) records feature coverage and
known limits.

## Build and first result

Use a recursive checkout of this repository with access to its private submodules. Install
Clang 17 or newer (Homebrew LLVM on macOS), CMake 3.28 or newer, Ninja, OpenSSL, zlib, and
Python 3 for the documentation checks. Linux also requires libfuse3. Configuration downloads
pinned source dependencies; the AppImage runtimes in
[`third_party/appimage-runtimes/`](third_party/appimage-runtimes/) are vendored and hash-checked,
so that input does not require network access. See
[local prerequisites](docs/build/local.md#prerequisites) for details.

From the checkout root, build the CLI and record a file in a fresh example directory:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja && cmake --build build -j 6
mkdir build/readme-example
printf 'first line\n' > build/readme-example/notes.txt
(
    export LIQUID_HOME="$PWD/build/readme-identity"
    cd build/readme-example
    ../liquid init .
    ../liquid cut "first version"
    ../liquid log
)
```

`init` imports `notes.txt`; `cut` prints `change=<digest>`; `log` shows that digest and
`first version`. The example keeps its repository in `build/readme-example/` and its
local signing identity in `build/readme-identity/`. The static library is `build/libliquid.a`
and the CLI is `build/liquid`.

To package ordinary files, follow [package repository files](docs/guides/project-from-files.md).
It requires no build configuration or filesystem mount.

Continue with [your first repository](docs/guides/first-repository.md) to capture an edit,
compare changes, search, and sync with a local peer. To try build caching, follow
[wrap an existing build](docs/guides/wrap-an-existing-build.md). The wrapper requires libfuse3
on Linux or macFUSE on macOS; the repository example above needs no FUSE mount.

## Documentation

[The documentation index](docs/README.md) organizes the full tree by topic.

| I want to… | Start here |
|---|---|
| Check implemented features and limits | [Implementation status](docs/library/status.md) |
| See what a release contains | [Releases](docs/releases/README.md) |
| Follow a workflow | [Executable guides](docs/guides/README.md) |
| Look up a command or flag | [Generated CLI reference](docs/reference/cli/README.md) |
| Configure, test, or install liquid | [Local build](docs/build/local.md) and [CI coverage](docs/build/ci.md) |
| Use the library and handle errors | [libliquid](docs/library/libliquid.md) and [public API map](docs/library/api-map.md) |
| Understand the design and vocabulary | [Feature specifications](docs/features/README.md) and [research synthesis](docs/research/99-synthesis/README.md) |
| Understand the documentation reconciliation | [Historical context and decisions](docs/library/readme-history.md) |

## Contributing and project context

liquid is an Orama project. The [liquid bridge](https://github.com/oramasearch/liquid-bridge)
holds the action plan; work is tracked from its
[master epic](https://github.com/oramasearch/liquid-bridge/issues/7).
[lake](docs/features/18-lake-service.md) is the hosted-service project and
[liquify](docs/features/21-liquify-ide.md) is the IDE project.

Read [CLAUDE.md](CLAUDE.md) and [AGENTS.md](AGENTS.md) before contributing. Feature and status
detail belongs under `docs/`; keep this page focused on the first useful workflow.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) holds the repository rules: the vocabulary, the document structure, the
checks to run before finishing, how a release is cut, the recorded decisions, and the org
conventions inherited from [`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge).
[`AGENTS.md`](AGENTS.md) restates the non-negotiables for every agent.

## Licence

The open-source licence has not yet been chosen
([release decision](https://github.com/oramasearch/liquid-bridge/issues/2)).
Third-party notices are in [NOTICE](NOTICE).
