# Importing the Spack package universe

**What.** Issue #75 adds the first foreign package-universe importer. A Python exporter runs under
a pinned Spack checkout and produces `spack-export/1`; libliquid synchronously parses that document,
fetches any digest-less patch through the shared coroutine runtime, and writes canonical `.liquid`
documents plus `package/1` records and `import-report/1`.

**Why Spack first.** Liquid's package identity, variants, conditional dependencies, virtual
providers, conflicts, patches, and spec syntax already follow Spack's model. That makes Spack the
lowest-loss source. Importing nixpkgs would first require evaluating the Nix language, while
conda-forge recipes preserve less of the source build structure needed by liquid. Both remain
possible later importers; neither is part of this implementation.

## Trust and execution boundary

`tools/spack-export/export.py` runs as `spack python export.py` inside the selected Spack checkout.
The checkout is a digest-pinned input to a wrapped liquid action, and the action result carries
`slsa_provenance/1`. The export records the checkout digest, Git revision, roots, and the provenance
carrier. Spack and Python are import-time tools only: no Spack package code or Python interpreter is
linked into or invoked by libliquid.

Only literal return values from `cmake_args` and `configure_args` are exported as static arguments.
Any Python control flow or computed return value is a `dynamic_value` loss. The same rule applies
to callable variant values and package methods that compute source URLs.

## Export schema and mapping

The exporter writes one JSON document with schema `spack-export/1`. Packages and fields are sorted
where Spack's API does not already define order.

| Spack export | Liquid output | Translation rule |
|---|---|---|
| version, URL, SHA-256 | package version plus `source/1` | URL and digest must both exist; non-HTTPS URLs remain a reported loss, and source bytes are verified when fetched |
| variant values, default, `when` | variant definition | scalar values translate; a condition must parse as a liquid spec |
| dependency constraint, `when`, types | dependency definition | virtual identity is retained; link, then run, then build precedence is reported when Spack names multiple types |
| virtual provide and range | `provides` | provider name and version range are preserved |
| conflict and message | conflict definition | the constraint and optional condition must parse as specs |
| patch URL, SHA-256, `when` | conditional patch digest | a missing digest causes an HTTPS fetch and SHA-256 pin during import |
| build-system class | build target | CMake, Autotools, and Makefile packages lower to ordinary configured targets |
| literal configure/CMake arguments | target command | each argument is quoted as one shell argument |
| exporter loss | package report entry | kind, field, and reason are retained verbatim |

Each package is written beneath `packages/<first-character>/` twice: the formatter produces
`<name>.liquid`, and the canonical record codec produces `<name>.cbor` for the universe loader.
Source fetch first checks the repository's content store, allowing an already verified digest to
be reused without network access.

## Loss report

`import-report/1` records every package, translated field names, per-field losses, roots, and total
counts. The stable loss categories are:

- `dynamic_value`: Spack computes the value in Python at build time;
- `custom_phase`: a package adds a build phase outside the standard class;
- `untranslatable_when`: a condition is not representable by the spec grammar;
- `unknown_build_system`: the package does not derive from the supported build classes;
- `unsupported_value`: the source value cannot be represented without changing its meaning.

Packages with losses are still imported when their package identity remains usable. The report
keeps an entry but writes no package record when none of its version names can be represented by
`package/1`; the package total counts records actually written. The report does not claim that a
package builds. `liquid import spack --report` prints the human-readable form and `--json` prints
the stable JSON form for tooling.

The solver accepts at most 4,096 configurations for one package. When Spack's Cartesian variant
space exceeds that bound, the importer retains defaults for later variants and reports every set of
non-default values it omitted. The pinned fixture writes 540 package records and reports 4,373
losses under this rule.

## Universe fixture and resolution

The committed root list is:

```text
hdf5+mpi
netcdf-c
openblas
fftw
python
cmake
boost
openssl
```

`tests/import/fixtures/spack-560.json` is the output of the documented roots from pinned Spack
revision `2bfcc69fa870d3c6919be87593f22647981b648a`, whose archived checkout has SHA-256
`68dc36928d212cf0197b958bba4c0e081270534f856a8d3fde0dab6da170e648`. It contains 560 source
packages; 540 have at least one version representable by `package/1`, and the rest remain explicit
report losses. This fixture proves the exporter and importer against Spack's real API.

`tests/import/fixtures/spack-500.json` is the deterministic scale fixture. It preserves the same
schema and root names while replacing Spack's changing package repository with a stable dependency
chain. The test imports and resolves exactly 500 packages with variants. It also requests
`hdf5~mpi ^openmpi`; the HDF5 conflict yields the normal attributed unsatisfiable narrative.

The twenty-package fixture covers versions, variants, conditional and virtual dependencies,
provides, conflicts, patches, three supported build classes, and all loss paths. Its formatter
output and report are committed as goldens. The `.invalid` source and patch URLs are deliberate:
tests preseed source digests and inject exact patch bytes, so default suites never require network
access.

## Linux build subset

The opt-in `realworld` test imports and lowers these ten leaf and near-leaf targets:

```text
zlib bzip2 xz expat libffi pcre2 libpng sqlite ncurses readline
```

Each target takes its source by SHA-256, executes through `build_configuration`, publishes
`out/prefix`, and must pass the relocation publication check. The test uses the Linux build fixture
as its toolchain and preseeded source content, so it verifies the importer-to-lowerer-to-action
path without downloading upstream projects. Packages outside this subset remain represented by
their report fields and losses; the importer does not hide unsupported build behavior.

Configure the Linux lane with `-DLIQUID_SPACK_REALWORLD_TESTS=ON` and run:

```sh
ctest --test-dir build-linux -L realworld -R spack-import-build-subset --output-on-failure
```

## Public API

`include/liquid/import.hpp` exposes standard-library report values, synchronous report codecs, and
`import_spack(...)`. Parsing and formatting are synchronous. The import operation is a Capy task
because digest-less patch retrieval and file writes may wait; it uses the shared runtime and never
starts raw threads. `spack_import_options::patch_contents` lets offline callers supply exact patch
bytes, while `toolchain` binds generated targets to a content-addressed toolchain.

The command-line adapter is in `src/cli/git.cpp`; the importer implementation is isolated in
`src/import/spack.cpp` and is part of `libliquid`.
