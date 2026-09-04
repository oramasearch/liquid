# REAPI protobuf codec

Issue [liquid #156](https://github.com/oramasearch/liquid/issues/156) implements the protobuf half
of the REAPI client. The implementation is internal to libliquid under `src/protobuf/` and
`src/reapi/`; it adds no protobuf runtime, generated source, gRPC dependency, or public header.
HTTP/2 framing and the client that implements `liquid/remote.hpp` are separate work.

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
above, all wire forms, malformed input, and unknown-field retention.
