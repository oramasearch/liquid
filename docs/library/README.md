# Library implementation notes

The [documentation index](../README.md#library) lists one entry for each library topic.
[Implementation status](status.md) is the authoritative inventory of implemented behavior and
known limits. [libliquid](libliquid.md) covers CMake targets, the installed dependency closure,
consumer setup, and the repository/coroutine install gate; the [API map](api-map.md) defines
the public headers and ABI rules.
The [documentation index](../README.md#library) lists the library's implementation pages.
[Status](status.md) records what is implemented and the [API map](api-map.md) defines the public
surface. The [identity guide](identity.md) covers public document export for sign-up, the shared
canonical-CBOR codec, software and hardware custody, and verification.
The [transparency guide](transparency.md) records checkpoint, tile, inclusion, and timestamp
verification, including the CLI log-inclusion policy.
# Library documentation

The [documentation index](../README.md#library) lists the public API and implementation pages.
[Status](status.md) records implemented behavior and limits.

[Standalone splits](split.md) describes owned data, retained original history, nested repository
identity, failure cleanup, and the issue #2 verification.
# Library implementation notes

[`status.md`](status.md) records the implemented behavior and remaining limits.
[`libliquid.md`](libliquid.md) describes the public CMake targets, installed dependency closure,
consumer setup, and repository/coroutine install gate. [`api-map.md`](api-map.md) defines the
public headers and ABI rules.

The complete topic index is in [`../README.md`](../README.md#library).

[VFS conformance](vfs.md#backend-conformance-301) documents named backend execution, common
filesystem and observation assertions, direct core calibration, adding a driver, and the Linux
libfuse baseline still owed against #299. The [proof record](vfs-conformance-proof.md) preserves
the passing cases, deliberate regression and native suite results.
The [identity guide](identity.md) covers public document export, custody, and verification.
[Standalone splits](split.md) covers data ownership, retained history, identities, and cleanup.
The [historical README record](readme-history.md) preserves earlier context and documents how
competing front-door and index passages were reconciled.
The hot-update status includes bounded demo socket paths and explicit platform-limit diagnostics;
[the fixture guide](../build/socket-fixtures.md) records the construction and cleanup rules.

[Projection](projection.md) includes ordinary-repository changes and trees, the public tree-digest
command, vendored AppImage runtime provenance, and the
[issue #286 verification record](../build/project-ordinary-repository.md).
The [HTTP log URL contract](transparency.md#http-url-validation-289) documents factory errors,
legacy-constructor behavior, CLI exit statuses, and regression coverage with networking denied.
[Collection's publication order](gc.md) puts the object map before the tombstone manifest, so a
crash between them reopens cleanly (liquid #337).
