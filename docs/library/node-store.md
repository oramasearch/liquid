# Node store

**Status: implemented for liquid #202.**

The node store is one long-running liquid component on every runtime node. It owns one shared
chunk store under `--root`, exposes read-only image roots to the local container runtime, and
hydrates only chunks the node does not already have. An image identifies its root with an
`io.liquid.tree` annotation and its source repository with an `io.liquid.store` config value.

The containerd proxy snapshotter belongs to k0s issue #14. It asks this component for the image
root, bind-mounts the returned path, and supplies its own writable upper directory. liquid does
not implement a snapshotter, create writable layers, or project OCI images in this issue.

## Layout and lifecycle

`liquid node-store serve --root <path>` owns this layout:

```text
<root>/store/                  one shared liquid chunk store
<root>/images/<tree>/          per-tree repository metadata over the shared store
<root>/mounts/<tree>/          read-only VFS mount returned to the snapshotter
```

The process opens the store before either listener becomes ready. The default snapshotter
listener is `unix:///run/liquid/node-store.sock`; the default operator control listener is
`127.0.0.1:8082`. Readiness requires the store to be open and the Unix socket to be bound.

A successful mount performs these operations on the runtime executor:

1. Parse and retain the requested tree digest, store URL, and optional bearer credential.
2. Install one `sync_endpoint` per distinct store URL in the shared store's remote-source map.
   The source id is the URL and the sync instance is the same URL.
3. Run `sync_session::receive_metadata({tree})`. The session fetches tree and blob records only,
   stores those records locally, and registers each blob's whole-content digest as remote with
   `{source: store_url, instance: store_url}`. No content chunk is fetched by this step.
4. Create or open `<root>/images/<tree>` on the caller-owned shared store and call
   `repository::record_from_tree` for the requested image root.
5. Mount the repository at `<root>/mounts/<tree>` with `mount_options::read_only = true`.

Mounting an already-mounted tree returns the same path and increments its reference count.
Unmounting by tree or path decrements that count and releases the VFS mount only at zero. A
request that supplies neither selector, or supplies selectors for different mounts, is invalid.
Process restart reconstructs repositories from their per-tree metadata; active kernel mounts and
reference counts are not claimed durable.

`prefetch <tree>` walks every blob below a mounted tree and reads its whole content through the
store. It therefore verifies and hydrates every missing chunk without changing the read-only
mount or its reference count.

## Accounting

The node store reports one entry per mounted tree:

- `chunks_fetched` and `bytes_fetched` count verified remote content chunks hydrated by VFS reads
  or prefetch;
- `bytes_avoided` is the sum of that tree's chunk sizes whose content was already local when the
  tree was mounted;
- `hydrated_bytes` is the VFS mount's logical byte counter;
- `references` is the number of successful mounts not yet released.

Shared chunks count as avoided for each image that can reuse them, but occupy bytes once in the
node store. `store_bytes` is the deduplicated logical size of live indexed chunks; obsolete pack
frames do not keep a budget above its target while compaction is pending. The component reports the
configured budget, successful evictions, and the most recent operational error.

## Budget and eviction

`budget_bytes == 0` disables automatic budget enforcement. For a positive budget, the node store
checks the logical live size after every hydration and prefetch. While the store remains over
budget, it selects the least-recently-read eligible blob content and calls `store::evict`.

Eligibility is deliberately narrow: the digest must be registered remote and currently local.
Tree and blob records reachable from a mounted image are pinned, and a digest without a remote
registration is never selected. Mounted image content may be evicted because its registration
makes later verified hydration possible; mounted metadata may not. Content remains an eviction
candidate after its last mount is released. Reads and prefetch update the same recency table.
Explicit `POST /v1/evict` applies the identical eligibility checks.

Eviction retains the remote registration, so a later read hydrates the content again. Failure to
meet a budget because no eligible candidate remains is reported as an error and leaves all
non-eligible content intact.

## Listener boundary

The Unix socket is local to the node and is the only endpoint the containerd proxy snapshotter
uses. It carries HTTP/1.1 JSON:

| Method | Path | Caller and result |
|---|---|---|
| `POST` | `/mount` | Snapshotter sends `{tree, store_url, bearer}` and receives the mount result. |
| `POST` | `/unmount` | Snapshotter sends `{tree}` or `{path}` and receives an empty success object. |
| `POST` | `/prefetch` | Snapshotter or local operator sends `{tree}` and receives updated status. |
| `GET` | `/status` | Local diagnostics receive the complete node-store status. |

The TCP control listener is for operators and admission plumbing. It exposes health, readiness,
Prometheus metrics, REST, OpenAPI, and MCP:

| Method | Path | Contract |
|---|---|---|
| `GET` | `/healthz` | The component process and control listener are healthy. |
| `GET` | `/readyz` | The shared store is open and the Unix listener is bound. |
| `GET` | `/metrics` | Per-tree fetch/avoid counters plus store, budget, and eviction totals. |
| `GET` | `/v1` | Complete `node_store_status` JSON. |
| `GET` | `/v1/mounts` | The status mount array. |
| `GET` | `/v1/attestations?subject=<digest>` | Canonical stored attestation bytes, base64 encoded. |
| `POST` | `/v1/evict` | Explicit eligible-content eviction for `{digest}`. |
| `POST` | `/v1/budget` | Replace the byte budget with `{bytes}` and enforce it. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 description of this listener. |
| `POST` | `/mcp` | MCP JSON-RPC with `node_store_status`, `node_store_mounts`, and `node_store_evict`. |

Mount and unmount are never registered on TCP. The scheduler, worker, and node store use one
small HTTP route table under `src/serve/`; scheduler and worker behavior stays unchanged.

The listener accepts only one bounded request per connection and closes it after the response.
Both listeners use Corosio acceptors and sockets on the caller's runtime. Store access and remote
hydration remain Capy tasks. Blocking filesystem metadata and FUSE work use `runtime::offload`,
and FUSE callbacks continue through the VFS request queue.

## Failure behavior

- If the store URL is unreachable but all tree and blob records are already local, mount succeeds.
  A later read of a missing chunk fails with `EIO`; `last_error` names the failed URL and the
  underlying liquid error.
- If any required tree or blob record is absent and the store URL is unreachable, mount fails with
  `errc::unavailable`; the error context names that URL. No partial mount or reference is exposed.
- A corrupt remote record or chunk fails digest verification and is never published as local.
- An unknown tree, mount, or prefetch target returns `errc::not_found`.
- An invalid digest, malformed JSON, conflicting unmount selectors, or unusable listen address
  returns `errc::invalid_argument`.
- Failure to mount through FUSE reports the platform error. macOS without macFUSE reports the
  existing documented `errc::unsupported` result.

## Command line and deployment

The server command is:

```sh
liquid node-store serve --root /var/lib/liquid-node-store \
  --listen unix:///run/liquid/node-store.sock \
  --control-listen 127.0.0.1:8082 \
  --budget-bytes 68719476736
```

`LIQUID_NODE_STORE_ROOT`, `LIQUID_NODE_STORE_LISTEN`, `LIQUID_CONTROL_LISTEN`, and
`LIQUID_NODE_STORE_BUDGET_BYTES` provide defaults; flags win. Operator clients are `node-store
status [--control <addr>] [--json]`, `mount <tree> --store <url> [--socket <path>]`, `unmount
<tree>`, and `prefetch <tree>`.

The image under `deploy/node-store/` includes libfuse3. It needs `/dev/fuse` plus the mount
capability, or placement in the snapshotter's mount namespace. The immutable node image tracked by
k0s issue #16 must also ship libfuse3.

The command includes the explicit `serve` subcommand. This is the spelling selected by liquid
#202; k0s issue #14 must use it even where its earlier brief omitted `serve`.

## Verification

The proof serves two deterministic 64 MiB trees from an in-process `sync_server`. Tree B differs
from tree A by one 4 MiB file. The test mounts and reads every file from A, then mounts and reads
every file from B. It records both pull counts and asserts B fetches no more than the changed file's
chunk bytes plus one chunk boundary while `bytes_avoided` covers the unchanged bytes.

The same suite proves metadata-only receipt fetches no chunks, repeated mount references one path,
prefetch hydrates every chunk, eviction reaches the budget without touching non-remote content,
both unreachable-store cases, every control route and metric, attestation lookup, and the built CLI.
Linux runs the FUSE path in the privileged container proof. macOS records the existing explicit
skip when macFUSE is unavailable.

Measured pull counts and the host load average are added here by the completed proof.
