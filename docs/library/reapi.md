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
| ByteStream | `Read`, `Write`, and `QueryWriteStatus`; large writes resume from the committed offset. |
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
`.liquid-store/reapi-action-cache`. The server intentionally does not offer Execution; liquid's
worker and scheduler are tracked separately.

The server accepts HTTP/2 prior-knowledge h2c. Its initial SETTINGS frame precedes its acknowledgement
of the client preface, which is required by strict stock clients. TLS termination is expected in
front of this server command.

## Configuration and CLI

`repository_options::remote` persists the following `remote_config` fields in `config.remote`:

| Field | Meaning | Default |
|---|---|---|
| `endpoint` | `host:port`, `http://host:port`, or `https://host:port` | required |
| `instance_name` | REAPI instance prefix | empty |
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

## Public API map

| Header | Surface |
|---|---|
| `liquid/remote.hpp` | `remote_config`, TLS configuration, `remote_blob`, the transport-neutral source and executor contracts, and the move-only `remote` REAPI handle. |
| `liquid/reapi.hpp` | `reapi_serve_options` and `serve_reapi`. |
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
