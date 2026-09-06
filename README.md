# liquid

liquid is a content-addressed, coroutine-native version control system and build system in one
program. One chunk store holds source, binaries, packages, images, search indexes and encrypted
data. One dependency graph of pure actions builds, packages, indexes and deploys from that store;
the sandbox observes what each action reads rather than trusting a declaration, and outputs are
content-addressed, so a rebuild that produces identical bytes stops the cascade. Every write to
disk is captured as a signed drip, and nothing is materialised until it is read. On top of the
store sit the incremental query graph, dependency resolution with liquid's own solver, and
projection of one checked install tree to native packages, ecosystem recipes, images, and bundled
runtime formats. The premise, the vocabulary, the design axioms and what liquid must do are stated
in [`docs/features/README.md`](docs/features/README.md).

liquid is the tool. [lake](docs/features/18-lake-service.md) is the hosted service where it
collects, and [liquify](docs/features/21-liquify-ide.md) is the IDE. The repository is an Orama
project and one module of
[`oramasearch/liquid-bridge`](https://github.com/oramasearch/liquid-bridge), which holds the
action plan.

## Status

The Phase 1 store exit criterion is met: the chunk store, repositories, capture, identity,
encryption, Git import and export, sync, drive and the macOS/Linux checkout daemon are implemented
inside libliquid. The Phase 2 build wrapper, cache, verification and provenance, the Phase 3
remote blobs, replay and delta delivery, the Phase 4 query graph, daemon, search and structural
edits, and the Phase 5 formats, projection, resolution and `.liquid` configuration are implemented
as recorded in [`docs/library/status.md`](docs/library/status.md), which also lists the known
limits. The code is a C++26 library, `liquid::liquid`, plus the `liquid` binary as a thin client
of it.

## Build and run

liquid needs a modern Clang (Homebrew LLVM on macOS, `clang++-17` or newer on Linux), CMake 3.28
or newer, Ninja, OpenSSL and zlib. Linux also needs libfuse3. On macOS, macFUSE is optional and
enables lazy mounts and `liquid wrap`.

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
cd liquid
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build
```

The static library is `build/libliquid.a` and the command-line binary is `build/liquid`. A first
repository:

```sh
mkdir example && printf 'first\n' > example/readme.txt
(cd example && ../build/liquid init . && ../build/liquid status --json && ../build/liquid cut "import")
```

`cmake --install build --prefix <prefix>` installs `bin/liquid`, `lib/libliquid.a`,
`include/liquid/` and the CMake package. Prerequisites, options, the install layout and the
documentation checks are in [`docs/build/local.md`](docs/build/local.md). Consuming the library
from another CMake project is in [`docs/library/libliquid.md`](docs/library/libliquid.md).

## Documentation

[`docs/README.md`](docs/README.md) indexes every page. The most useful ones:

- [`docs/features/README.md`](docs/features/README.md): the premise, names, design axioms, what liquid must do, and the 25 feature areas.
- [`docs/library/status.md`](docs/library/status.md): what is implemented today, by phase, and the known limits.
- [`docs/library/api-map.md`](docs/library/api-map.md): the public headers of libliquid and the rules they obey.
- [`docs/cli/README.md`](docs/cli/README.md) and [`docs/reference/cli/`](docs/reference/cli/): the command-line client and its generated reference.
- [`docs/guides/`](docs/guides/): executable workflows for a first repository, a wrapped build, and keys and encryption.
- [`docs/research/99-synthesis/`](docs/research/99-synthesis/): the argument in one place, drawn from the 27 research topics.

## Repository rules

[`CLAUDE.md`](CLAUDE.md) holds the repository rules: the vocabulary, the document structure, the
checks to run before finishing, the recorded decisions, and the org conventions inherited from
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge).
[`AGENTS.md`](AGENTS.md) restates the non-negotiables for every agent.

## Licence

liquid and liquify will be open source; the licence text is not yet chosen
([liquid-bridge #2](https://github.com/oramasearch/liquid-bridge/issues/2)). Until then
`oramasearch/liquid` is private. Third-party notices are in [`NOTICE`](NOTICE).
