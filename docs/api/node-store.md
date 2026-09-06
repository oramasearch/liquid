# Node store API

The node store has two HTTP/1.1 listeners. The local Unix socket defaults to
`/run/liquid/node-store.sock` and is the snapshotter boundary. The TCP control listener defaults
to `127.0.0.1:8082` and is the operator and admission boundary. Both accept one bounded request
per connection and return JSON errors with `code`, `message`, and `context`.

## Local snapshotter listener

| Method | Path | Request | Response |
|---|---|---|---|
| `POST` | `/mount` | `{"tree":"<digest>","store_url":"<sync URL>","bearer":"<optional>"}` | `path`, `tree`, `records_fetched`, and `bytes_remote` |
| `POST` | `/unmount` | `tree`, `path`, or both when they select the same mount | `{}` |
| `POST` | `/prefetch` | `{"tree":"<digest>"}` | Complete status after hydration |
| `GET` | `/status` | none | Complete status |

Repeated mounts of one tree return one path and increment `references`. Unmount releases the VFS
mount at zero. Mount and unmount are intentionally absent from TCP.

## TCP control listener

| Method | Path | Result |
|---|---|---|
| `GET` | `/healthz` | Process liveness. |
| `GET` | `/readyz` | Ready after the shared store opens and the Unix socket binds. |
| `GET` | `/metrics` | Prometheus counters and gauges described below. |
| `GET` | `/v1` | Complete status. |
| `GET` | `/v1/mounts` | The status `mounts` array. |
| `GET` | `/v1/attestations?subject=<digest>` | Stored attestation digest and canonical bytes as base64. |
| `POST` | `/v1/evict` | Evict registered local remote content from `{"digest":"<digest>"}`. |
| `POST` | `/v1/budget` | Set and enforce the logical byte budget from `{"bytes":<integer>}`. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 route inventory. |
| `POST` | `/mcp` | MCP initialization, tool listing, and tool calls. |

Status fields are `store_bytes`, `budget_bytes`, `evictions`, `mounts`, and `last_error`. Each
mount has `tree`, `path`, `references`, `chunks_fetched`, `bytes_fetched`, `bytes_avoided`, and
`hydrated_bytes`.

The MCP tools are `node_store_status`, `node_store_mounts`, and `node_store_evict`. The Prometheus
surface is:

```text
liquid_node_store_chunks_fetched_total{tree="<digest>"}
liquid_node_store_bytes_fetched_total{tree="<digest>"}
liquid_node_store_bytes_avoided_total{tree="<digest>"}
liquid_node_store_store_bytes
liquid_node_store_budget_bytes
liquid_node_store_evictions_total
```
