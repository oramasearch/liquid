# Repository sync protocol

**What.** The HTTP/1.1 protocol used by libliquid clients and lake to exchange repository
metadata and content. lake's `docs/api/sync.md` mirrors this page.

**Status: implemented by liquid #160 and #165.** Metadata export and import are bounded by
caller-provided spans. Imports verify the complete batch before changing repository state. Content
negotiation and transfer reuse `remote_blob_source` and the store's pack-entry framing. The public
`sync_server` owns protocol decoding and repository dispatch; `liquid serve sync` is its local
HTTP/1.1 adapter.

## Repository URL and requests

The URL passed to `liquid push` or `liquid pull` identifies one repository. Clients append the
paths below. Requests and responses use `Content-Type: application/vnd.liquid.sync-v1` and may use
either `Content-Length` or HTTP/1.1 chunked transfer coding. Frame boundaries are independent of
HTTP read boundaries.

| Method and path | Request body | Successful response |
|---|---|---|
| `POST <repository>/sync/push` | client metadata stream | acknowledgement header, optionally followed by server metadata the client must import |
| `POST <repository>/sync/pull` | one header frame containing the client cursor and refs | metadata stream exported after that cursor |
| `POST <repository>/sync/find-missing` | one header frame containing `objects` | one header frame containing the missing subset |
| `POST <repository>/sync/chunks` | zero or more pack-entry frames | empty body |
| `GET <repository>/sync/chunks/<digest>?offset=<n>&length=<n>` | none | raw verified logical bytes for the requested range |

The metadata header is a canonical-CBOR map with `v: 1`, `cursor`, sorted `refs`, and sorted
`objects`. A push header cursor is the end of the exported client journal. A pull request cursor is
the last server cursor observed by that client. A response cursor is the next opaque cursor to
persist for that peer.
`objects` contains the roots referenced by the metadata stream. Missing-object negotiation walks
their content-addressed graph and sends only objects the receiver reports absent.

An optional `Authorization: Bearer <token>` header carries lake authentication. The CLI reads the
token from `LIQUID_TOKEN`; credentials never appear in the URL or output.

## Metadata stream

Every metadata frame has a one-byte type, a four-byte unsigned big-endian payload length, and that
many payload bytes. Receivers reject unknown types, a length above 16 MiB, truncated frames, bytes
after the end frame, or a stream without an end frame.

| Type | Name | Canonical-CBOR payload |
|---:|---|---|
| 1 | header | the version, cursor, refs, and object roots described above |
| 2 | drip | `{author, body, signature}`; `body` is the exact signed canonical drip body |
| 3 | change | `{author, id, body}`; signed change bytes, digest, and verifiable TAAP key |
| 4 | directory | `{repository, node, root, message}`; two 16-byte IDs, a root marker, and an automerge v1 sync message |
| 5 | ref | `{name, change}` |
| 255 | end | empty |

Drips retain journal order. A change follows the drips it cuts. Directory messages use the
existing checked CRDT receive path and are associated with their directory by `node`. Ref frames
are applied last. An empty repository adopts the incoming repository and root IDs; a non-empty
repository with a different identity rejects the batch. Duplicate signed records and
already-observed CRDT messages are valid, making a retry idempotent.

Repository checkpoints keep compact bytes that can be opened either as the shallow active
document or as its full-history archive. Import opens a candidate from the full archive before
receiving peer messages. A retry can therefore name a dependency older than the active checkpoint
heads without contradicting the shallow document's retained actor sequence.

Before import changes state, the receiver parses every frame, verifies each actor identifier and
TAAP signature, checks each change digest and referenced object, applies every directory message to
candidate documents through the checked patch-returning path, and validates all refs. Any failure
rejects the whole batch. The error `item` is the zero-based metadata-frame index that failed.

## Chunk stream

Chunk uploads concatenate the store's unencrypted physical pack-entry frames without a pack file
header, table, or footer:

```text
8 bytes  "LQFRAME1"
4 bytes  uncompressed length, little-endian
4 bytes  compressed length, little-endian
1 byte   digest algorithm
32 bytes digest
N bytes  zstd payload
```

The receiver bounds both lengths, decompresses the payload, verifies its algorithm-tagged digest,
and stores the logical bytes through the ordinary store path. Trust-domain encryption, when
configured, is applied by the receiving store; encrypted physical frames are never sent. A chunk
range response is logical content rather than a partial compressed frame and is verified as part
of the complete object read.

## Errors

Non-success responses use `Content-Type: application/vnd.liquid.error-v1` and a canonical-CBOR map
`{v: 1, code, message, context, item?}`. `code` is the stable `liquid::errc` name.

| HTTP status | liquid error |
|---:|---|
| 400 | `invalid_argument`, `corrupt` |
| 401 | `unauthorized` |
| 404 | `not_found` |
| 409 | `already_exists`, `conflict` |
| 423 | `locked` |
| 429 | `busy` |
| 503 | `unavailable` |
| 500 | `io`, `internal` |

An interrupted request has no success response and is retried from the last acknowledged cursor.
Servers do not advance that cursor until metadata import and required chunk publication both
succeed.

## Public server and client contract

`sync_server(repository&, store&, options)` requires the repository to use that store. The
repository-only constructor uses the repository's existing store and is intended for adapters such
as the CLI. Its task-returning handlers accept or produce byte spans for push, pull, missing-object
queries, pack-frame uploads, and logical range reads; the canonical codecs remain private.
`sync_server::encode_error` produces the error body described above so an adapter need only choose
the transport status.

`sync_server_options::authorize` is an optional asynchronous per-change callback. Push invokes it
in frame order before calling repository import. A callback failure returns that error and leaves
repository metadata unchanged; object uploads that arrived earlier remain unreferenced
content-addressed data eligible for collection. Embedders can perform authorization and audit work
in this callback without reimplementing the wire format.

`repository::export_drips` writes as many `sync_item` values as fit in the supplied span and
returns the next cursor plus an end flag. `repository::import_drips` accepts a span, verifies it as
one atomic batch, and reports the number imported. `sync_session` sends or receives an object graph
through a `remote_blob_source` after `find_missing`. `sync_endpoint` implements that source and the
push/pull requests over the vendored `oramasearch/http` module. A caller may put its last observed
cursor in `sync_endpoint_options`; the returned report supplies the next cursor to retain. All
waiting operations are Capy tasks.

## Local server command

`liquid serve sync <path>` opens an existing repository and serves the five routes above with the
vendored `oramasearch/http` router and serializer. It binds `127.0.0.1:8080` by default;
`--listen <address>` and `--port <port>` select another listener. The command deliberately supplies
no authentication policy. Keep the loopback default for local exchange, or embed `sync_server`
with an authorization callback when exposing a service boundary.

## How to verify

The sync tests exchange two on-disk repositories through `sync_server`, prove that a second
exchange transfers only newly introduced chunks, retry unchanged metadata, and pull into an empty
repository. They also corrupt a TAAP signature and reject a change through the authorization
callback without changing repository state. The CLI test spawns `liquid serve sync` and runs the
real `liquid push` and `liquid pull` commands against it. Static, shared, and Linux-container proofs
run these tests with the rest of the suite.
