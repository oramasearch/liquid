# Source implementation

[`git/`](git/README.md) owns verified Git object decoding and the shared inflation and delta
budgets used by local import (#443).
`abi.cpp` carries binary-reader truncation into public descriptors and JSON. ABI comparisons
refuse compatibility claims from partial symbol lists, including descriptors loaded from storage.

`liquid split` documents owned destination storage, original signed history, nested support, and
failure cleanup in its typed command table. The implementation lives in libliquid.

`main.cpp` owns the public CLI command table and dispatches each command to its focused adapter
under `cli/`. Identity document export uses the public signed snapshot in `identity.cpp` and the
existing canonical-CBOR codec. Library implementation directories follow the installed public API areas while
keeping parsers, codecs, storage, network, solver, and runtime implementation types private.
Adapters under `cli/` include only `liquid/` and `cli/` headers (`cli-public-includes`):
`credential.cpp` exports the private `credential_token.hpp` bearer reader through
`liquid/credential.hpp`, and `liquid attest show` checks the issuer with
`check_attestation_signature`.
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
`daemon.cpp` hosts the checkout daemon. Its socket sits in a private per-user directory, is bound
under `umask(077)`, and serves only peers with the same uid; the rules are in
[the incremental engine](../docs/library/incremental-engine.md) (liquid#397).
HTTP log sources validate URL syntax and numeric port bounds locally. The factory returns
public errors; legacy sources return retained validation errors before transport access.
See [the URL contract](../docs/library/transparency.md#http-url-validation-289).

`fs/contained.hpp` holds the filesystem writes that never follow a symbolic link out of their
root. Install staging and both tree materialisers write only through it (liquid #385, #386), and
`objects/entry_name.hpp` holds the tree entry name rule they and the record codec share. See
[the object model](../docs/library/object-model.md#records-in-the-store) and
[install staging](../docs/library/projection.md#transactional-projected-tree-installation).
