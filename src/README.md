# Source implementation

`liquid split` documents owned destination storage, original signed history, nested support, and
failure cleanup in its typed command table. The implementation lives in libliquid.

`main.cpp` owns the public CLI command table and dispatches each command to its focused adapter
under `cli/`. Identity document export uses the public signed snapshot in `identity.cpp` and the
existing canonical-CBOR codec. Library implementation directories follow the installed public API areas while
keeping parsers, codecs, storage, network, solver, and runtime implementation types private.
The command table exposes `verify --log <log-url>` so callers can require transparency inclusion
for agreeing reproducibility attestations.
The projection implementation includes the Debian Tier B source writer and its committed,
peer-derived build-tool package mapping.

Spack package-universe import lives under `import/`; it is dispatched by the existing import
command group and exposed only through `include/liquid/import.hpp`.

`lake_transparency.cpp` owns the shared checkpoint, complete parent-tile, inclusion, consistency,
and identity-scan verifier. It accepts caller-pinned trust for admission and source-discovered trust
for the existing repository workflows.

Issue #287 connects source package declarations to universe loading. The CLI table documents
canonical package output through `config lower --output-record`; `resolve` reports an empty
universe before solving. See [resolution](../docs/library/resolution.md).

The command table documents ordinary-repository projection and `liquid tree`, which prints the
recorded root digest. The CLI chooses the owning repository before calling projection; format
writers retain their existing behavior.
