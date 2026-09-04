# REAPI protobuf codec

Issue [liquid #156](https://github.com/oramasearch/liquid/issues/156) implements the protobuf and
record-conversion half of the REAPI client. The implementation is internal to libliquid under
`src/protobuf/` and `src/reapi/`; it adds no protobuf runtime, generated source, gRPC dependency,
or public header. HTTP/2 framing and the client that implements `liquid/remote.hpp` are separate
work.

## Wire codec

`protobuf::reader` reads one field at a time without owning the input. `protobuf::writer` emits
canonical protobuf values. Together they cover:

- varint `int32`, `int64`, boolean, and enum values;
- zigzag `sint32` and `sint64` values;
- little-endian `fixed32` and `fixed64` values;
- strings, bytes, embedded messages, and packed repeated varints;
- skipped deprecated groups, including nested groups; and
- retention of the original bytes for unknown fields.

Each message struct has an `unknown_fields` byte vector. Decode copies complete unknown key/value
slices into it and encode appends those bytes after known fields. This preserves unknown values and
lets a message produced by the pinned reference runtime round-trip byte for byte. The reader rejects
field zero, invalid wire types, varints exceeding 64 bits, truncated fixed and length-delimited values,
unmatched group ends, and length values that exceed the remaining input.

The message layer also validates field wire types, UTF-8 strings, signed integer bounds, map
entries, packed and unpacked repeated enums, and protobuf `oneof` replacement. It represents maps
as ordered C++ maps so their emitted entries are deterministic.

## Message set

`src/reapi/messages.hpp` defines plain structs and encode/decode functions for the REAPI v2.3
surface used by a remote cache and executor:

- common messages: `Any`, `Timestamp`, `Duration`, `UInt32Value`, `Status`, `Operation`, and
  `SemVer`;
- Merkle and action messages: `Digest`, `DigestFunction`, `NodeProperties`, `FileNode`,
  `DirectoryNode`, `SymlinkNode`, `Directory`, `Tree`, `Platform`, `Command`, and `Action`;
- result and execution messages: `ActionResult`, `OutputFile`, `OutputDirectory`, `OutputSymlink`,
  `ExecutedActionMetadata`, `ExecuteRequest`, `ExecuteResponse`, `ExecuteOperationMetadata`, and
  `WaitExecutionRequest`;
- capabilities and CAS messages: `GetCapabilitiesRequest`, `ServerCapabilities`,
  `CacheCapabilities`, `ExecutionCapabilities`, `FindMissingBlobsRequest/Response`,
  `BatchUpdateBlobsRequest/Response`, `BatchReadBlobsRequest/Response`, and
  `GetTreeRequest/Response`;
- action-cache messages: `GetActionResultRequest` and `UpdateActionResultRequest`; and
- ByteStream messages: `ReadRequest/Response`, `WriteRequest/Response`, and
  `QueryWriteStatusRequest/Response`.

Nested protocol messages such as cache update support, priority ranges, compressors, log files,
execution policies, and batch request/response entries are represented as their own structs as
required by those top-level messages.

## Tree conversion

`reapi::DirectoryCache` converts a liquid tree record through caller-provided tree and logical-blob
loaders. Entries are sorted by name and divided into the three REAPI node lists. Regular files use
the digest of their logical contents, executable files set `is_executable`, symbolic-link blobs
become link targets, and child tree records become `DirectoryNode` values. Every node retains its
full Unix mode in `NodeProperties.unix_mode`.

Each encoded `Directory` is hashed and returned with its bytes. The cache key includes the liquid
tree digest, selected REAPI hash algorithm, and directory mode. Child results enter the cache as
they are completed, so a shared or unchanged subtree is not loaded or encoded again. Active tree
keys are tracked independently to reject cycles.

The committed reference has this shape:

```text
root/
  hello.txt       0100644  "hello\n"
  run.sh          0100755  "#!/bin/sh\necho hi\n"
  hello-link      0120000  -> hello.txt
  sub/            0040755
    nested.txt    0100644  "nested\n"
```

Python protobuf serializes the root `Directory` to `tests/reapi/fixtures/directory-reference.bin`.
Its SHA-256 digest is
`060b0d269f3e9e96094cec473650cce96bafe6fc5a2771a3723922bb587bab06`, which the conversion test
reproduces from liquid tree and blob records.

## Actions and results

An `action_input` maps to a `Command` plus `Action`. Arguments, environment, working directory,
declared outputs, and timeout use their standard fields. `OSFamily` and `ISA` carry the platform;
`liquid.inputs`, `liquid.toolchain`, and `liquid.sandbox` carry the native record digests needed to
reconstruct the input. Decode verifies the encoded `Command` against `Action.command_digest`
before producing a remote-eligible native action.

An `action_result` fills the standard output, exit, stdout/stderr digest, worker, timestamp, and
execution-duration fields. File and directory contents are hashed through a caller-provided logical
blob resolver. Symbolic-link contents become their target text. Liquid fields that REAPI cannot
represent, including the observed-input manifest and provenance references, are retained in a
`liquid.reapi.v1.ActionResultMetadata` `Any`. Its protobuf payload wraps the canonical native result
record. A Liquid peer recovers the complete record; another REAPI implementation can ignore that
auxiliary value and consume the standard fields. Decode also accepts results without the Liquid
metadata and constructs a native result from the standard fields.

## Digest handling

Capability selection prefers BLAKE3 and falls back to SHA-256 for both cache and execution
capabilities. No implicit conversion changes a liquid object's identity: the selected algorithm
hashes the bytes in the REAPI view and the resulting `Digest` carries the hexadecimal hash plus
byte length. Reverse conversion requires a 32-byte hexadecimal digest and restores the selected
algorithm tag. Other advertised digest functions are rejected.

## Reference fixtures

`tests/reapi/fixtures/MANIFEST` maps every binary fixture to its protobuf type. `VERSION` pins the
Remote APIs and googleapis commits and the Python protobuf runtime. `digest.bin` includes an unknown
field which the reference runtime parses and serializes before the fixture is written.

The generator also writes a two-level reference `Directory` and its SHA-256 root digest for the
native tree-conversion proof.

The fixture generator and its Dockerfile are committed under `tests/reapi/`, but neither is part of
the build. Regenerate the bytes from the repository root with:

```sh
docker build -t liquid-reapi-fixtures tests/reapi
docker run --rm -v "$PWD/tests/reapi/fixtures:/out" liquid-reapi-fixtures /out
```

The ordinary `liquid-tests` target reads the committed bytes. Tests cover every message named
above, all wire forms, malformed input, unknown-field retention, both supported digest algorithms,
subtree caching, the reference Merkle root, and native action/result round-trips.
