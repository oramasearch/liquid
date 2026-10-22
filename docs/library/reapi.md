# REAPI client and server

**Status.** liquid implements the Remote Execution API v2 cache and execution client over its
Corosio HTTP/2 transport and dependency-free protobuf codec. It also serves the repository store as
a SHA-256 REAPI ContentAddressableStorage, ByteStream, Capabilities, and ActionCache endpoint. The
implementation adds no third-party dependency.

## Client

`remote::connect(remote_config)` performs `GetCapabilities` and chooses BLAKE3 when advertised,
otherwise SHA-256. The REAPI view and liquid's native records retain separate algorithm-tagged
identities in the same store. The client implements:

| Service | RPCs and behavior |
|---|---|
| Capabilities | `GetCapabilities`; cache digest negotiation happens before any object request. |
| CAS | `FindMissingBlobs`, `BatchReadBlobs`, `BatchUpdateBlobs`; the configured batch limit selects the batch path. |
| ByteStream | `Read`, `Write`, and `QueryWriteStatus`; large writes resume from the committed offset and are sent on one stream as 1 MiB `WriteRequest` messages, never as one message (liquid#451). |
| ActionCache | `GetActionResult` and `UpdateActionResult`; publication is disabled unless `publish` is true. |
| Execution | `Execute` followed by `WaitExecution` when needed; operation streams are decoded until completion. A failed or expired wait sends `CancelOperation`. |

Every RPC carries `grpc-timeout`. The local Capy deadline also cancels resolver, connection, TLS,
HTTP/2, and response waits. Custom headers are marked sensitive for HPACK when they are credentials.
TLS supports system or explicit trust roots and an optional client certificate and key.
TLS connections require the peer to negotiate `h2` with ALPN before any REAPI request is sent.

Before execution, the input tree, toolchain tree, blob records, and content chunks are converted to
REAPI directories and uploaded only when absent. Results return as `action_result`; stdout, stderr,
and output blobs are verified while entering the local mixed-digest store. The observed-input
manifest travels in `ExecutedActionMetadata.auxiliary_metadata` and is stored in the native result.
The scheduler continues to use the transport-neutral `remote_blob_source` and `remote_executor`
interfaces.

## Remote cache tier

The disk tier is checked first. A remote ActionCache response is imported under the same candidate
key, signed locally, given local provenance and SBOM records, and passed through the ordinary disk
lookup again. That lookup verifies the candidate, signature, attestations, referenced objects, and
every observed manifest path against the current input tree before the result can be served.
The REAPI ActionCache digest is derived from the algorithm-tagged normalized disk candidate, so the
remote tier preserves the disk tier's command, environment, toolchain, sandbox, and policy identity
while the observed-input manifest remains responsible for validating the current input tree.

The default is read-only. `cache_stats`, `why`, `wrap`, and their JSON or text CLI forms expose disk
and remote hit, miss, transferred-byte, and avoided-byte counters separately. A first observation
that is not locally known to be remote-eligible does not query ActionCache. On Linux, the read guard
rejects a remote-eligible action's first access outside its input and toolchain roots before remote
dispatch.

## Server

Run the server from a liquid repository:

```sh
liquid serve reapi --listen 0.0.0.0:50051
liquid serve reapi --listen 127.0.0.1:50051 --instance builds
```

`serve_reapi(reapi_serve_options)` is the equivalent library entry point. The server offers
Capabilities, CAS `FindMissingBlobs`, batch reads and updates, `GetTree`, ByteStream reads,
client-streaming and resumable writes, and ActionCache reads and updates. CAS objects use standard
SHA-256 digest and size validation. ActionCache records live beside the repository store under
`.liquid-store/reapi-action-cache`, one file per action digest, named by its hash.

An action digest names a file only after it is checked (liquid #428, LSR-034 of the Lake
pre-launch security review): its hash must be exactly the lowercase hexadecimal encoding of a
digest of the request's `digest_function` (64 characters for SHA-256 and BLAKE3; the default is
SHA-256) and its size non-negative. Anything else, such as `../../file`, an absolute path, upper
case, or the wrong length, is refused with gRPC `INVALID_ARGUMENT` before the filesystem is
touched, by `GetActionResult`, `UpdateActionResult`, and the scheduler's `Execute`/`Submit`. Entries
are opened directly inside the cache directory with no symbolic link followed, at the directory
or at the entry, and an update writes a new file and renames it over the entry, so a link planted
under an entry's name is replaced and its target never opened. `src/reapi/action_cache.cpp`
implements both rules. The cache-only server intentionally does not offer Execution;
`liquid scheduler` reuses it with Execution and Operations enabled, as documented in
[`workers.md`](workers.md).

**Request limits (liquid#451).** The server never reads a request stream whole. It reads DATA
frames into a gRPC message reader that holds at most one message:

- A unary request is one message of at most 5 MiB: the 4 MiB `max_batch_total_size_bytes` it
  advertises plus 1 MiB for the message's own fields, so a full `BatchUpdateBlobs` fits.
- A ByteStream `Write` is read one message at a time, each of at most 5 MiB, and each is applied to
  the upload before the next is read. A blob of any size is uploaded as many messages on one stream
  (as Bazel and liquid's own client send it); one message larger than the limit is refused.
- A message whose five-byte prefix declares more than the limit is refused from the prefix, before
  any of its body is held, with gRPC `RESOURCE_EXHAUSTED` (8).
- A stream the server answers before the client has finished sending it (a refused or malformed
  request) is followed by GOAWAY and the connection is closed, so its remaining bytes are neither
  held nor queued. The client reconnects for its next call.

The bytes an unfinished upload keeps between Write calls (`partial_writes`) are a separate limit:
per upload, in total, by count and by idle expiry. That is
[liquid#494](https://github.com/oramasearch/liquid/pull/494) (liquid#444). Proof:
`tests/reapi/test_request_limits.cpp` and the `GrpcMessageReader.*` tests in
`tests/http2/test_codecs.cpp`.

The server accepts HTTP/2 prior-knowledge h2c. Its initial SETTINGS frame precedes its acknowledgement
of the client preface, which is required by strict stock clients. TLS termination is expected in
front of this server command.

## ByteStream upload limits (liquid #444, LSR-204)

Both `liquid serve reapi` and `liquid scheduler` retain unfinished uploads in memory so a
successful partial Write can resume on another connection. Each listener has one shared budget,
including all connections and instances. The defaults are:

| Library field in `reapi_upload_limits` | CLI environment variable | Default |
|---|---|---|
| `max_upload_bytes` | `LIQUID_REAPI_MAX_UPLOAD_BYTES` | 1 GiB (1073741824 bytes) |
| `max_buffered_bytes` | `LIQUID_REAPI_MAX_BUFFERED_BYTES` | 2 GiB (2147483648 bytes) |
| `max_uploads` | `LIQUID_REAPI_MAX_UPLOADS` | 128, including empty partial uploads |
| `max_resource_bytes` | `LIQUID_REAPI_MAX_RESOURCE_BYTES` | 1024 bytes |
| `idle_timeout` | `LIQUID_REAPI_UPLOAD_IDLE_MS` | 600000 milliseconds (10 minutes) |

Library callers set `reapi_serve_options::uploads` or `scheduler_options::uploads`. The CLI
variables accept positive decimal integers; invalid, zero, negative, or overflowing values refuse
startup. Library options require positive limits and a positive timeout, and the per-upload byte
limit must fit the protocol's signed 64-bit size. These settings affect ByteStream Write state,
not batch CAS updates or reads.

A declared upload larger than its limit is refused before any upload entry is created. Each
append must match the retained offset and fit the declared size. Count and total-byte exhaustion
return gRPC `RESOURCE_EXHAUSTED`; invalid resources, offsets, or sizes return `INVALID_ARGUMENT`.
The total counts allocated vector capacity, including spare capacity, replacement allocations
while the old buffer still exists, and completed buffers until storage and the response finish.
At the defaults this permits up to 2 GiB of upload payload allocations per listener, plus bounded
entry metadata. A growing upload can be refused when its replacement buffer cannot fit beside the
old buffer. The message being decoded and repository storage have their own memory use; this
budget is not a process RSS limit. The transport holds at most one Write message at a time
(liquid#451, above).

Only one Write can own a resource at a time. The owner releases its buffer on errors, failed
response delivery, or coroutine cancellation; digest and storage failures also release the charge.
A failure before a resource has been decoded creates no new retained upload. A successful partial
RPC releases ownership but keeps the bytes resumable. Normal disconnect after that response keeps
the partial upload until expiry. A finishing message must be last in its RPC: its bytes are stored
only after the stream has ended, and a message after it fails the Write with `INVALID_ARGUMENT`
without storing anything. A Write with no messages is refused.

A one-second sweep expires idle uploads even when no client makes another request. Writes and
status queries also prune expired entries. Appending and successfully acknowledging a partial
Write refresh its idle time; status polling does not. Active RPCs keep ownership and their budget
charge through storage and response delivery. The same timeout also bounds every wait of an active
Write: each next message, the storage of a finished upload, and the response. A Write that keeps
sending is not cut off however long it runs, but one that sends nothing for the timeout, or whose
response cannot be delivered within it, is cancelled: its upload owner releases the buffer and the
connection closes. This reclaims an upload when a client stops mid-stream or withholds HTTP/2
response flow-control credit. Once an unfinished upload expires,
`QueryWriteStatus` returns `NOT_FOUND`; resumption at its old offset fails, and the client must
restart at offset zero. Completed CAS objects remain available as before.

## Configuration and CLI

`repository_options::remote` persists the following `remote_config` fields in `config.remote`:

| Field | Meaning | Default |
|---|---|---|
| `endpoint` | `host:port`, `http://host:port`, or `https://host:port` | required |
| `instance_name` | REAPI instance prefix | empty |
| `backend` | stock `reapi` or `liquid_scheduler` execution path | `reapi` |
| `tls.enabled`, `tls.verify_peer` | TLS and certificate verification | false, true |
| `tls.certificate_authority` | optional PEM trust file | system trust |
| `tls.client_certificate`, `tls.client_key` | optional mTLS PEM files | none |
| `headers` | request metadata, including authorization tokens | empty |
| `deadline` | complete-RPC local and gRPC deadline | 60 seconds |
| `batch_blob_limit` | maximum blob size sent through batch RPCs | 4 MiB |
| `publish` | permit CAS and ActionCache publication | false |

`wrap` and `build --profile` accept `--remote`, `--remote-instance`, repeatable
`--remote-header name=value`, `--remote-tls`, `--remote-no-verify`, `--remote-ca`,
`--remote-client-cert`, `--remote-client-key`, `--remote-deadline-ms`, and
`--remote-publish`. Supplying the flags when a wrap repository is created persists the
configuration; later runs reuse it. CLI flags on a later run take precedence.
`wrap --scheduler <endpoint>` selects and persists the liquid scheduler backend while retaining
the same TLS, header, deadline, instance, and CAS settings.

## Public API map

| Header | Surface |
|---|---|
| `liquid/remote.hpp` | `remote_config`, TLS configuration, `remote_blob`, the transport-neutral source and executor contracts, and the move-only `remote` REAPI handle. |
| `liquid/reapi.hpp` | `reapi_upload_limits`, `reapi_serve_options`, and `serve_reapi`. |
| `liquid/workers.hpp` | scheduler and worker options plus their coroutine-native long-running entry points. |
| `liquid/repository.hpp` | `repository_options::remote` and `repository::remote_configuration`. |
| `liquid/build.hpp` | `wrap_options::remote`, per-tier `cache_report`, and the existing remote execution entry points. |

HTTP/2, gRPC, protobuf messages, and REAPI conversion types remain private implementation details.

## Interoperability proof

`tools/reapi-peers/run-reapi-interop.sh <build-directory>` starts pinned NativeLink and BuildBarn
containers. Against each peer it exercises negotiated CAS and ActionCache read/write, then runs the
Phase 2 Make wrap fixture with a remote-only second lookup. Both peers produce the local baseline's
100% second-run hit rate. The Linux read-guard fixture confirms `/etc/hostname` is refused before
remote dispatch.

The same script starts `liquid serve reapi` and runs the stock Bazel 7.4.1 client twice from clean
output bases. The first run writes CAS and ActionCache data; the second reports one remote cache hit.
The upstream `remote-apis` repository contains protocol definitions and test vectors but no server
conformance runner, and BuildBarn does not publish a separate black-box conformance binary. The
applicable upstream compatibility case is therefore the stock Bazel client read/write run, plus the
NativeLink and BuildBarn service round trips.

The complete platform proof remains the static and shared macOS builds and the privileged
`liquid-linux-ci` build described in [`../build/ci.md`](../build/ci.md).
