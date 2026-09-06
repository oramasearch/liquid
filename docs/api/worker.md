# Worker control API

The worker initiates every data connection to the scheduler. Its only inbound listener is the
HTTP/1 control endpoint.

| Method | Path | Result |
|---|---|---|
| `GET` | `/healthz` | `200` while the listener runs. |
| `GET` | `/readyz` | `200` after scheduler negotiation; otherwise `503`. |
| `GET` | `/metrics` | Prometheus text. |
| `GET` | `/v1` | `{"component":"worker","version":"v1"}`. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 route map. |
| `POST` | `/mcp` | MCP JSON-RPC discovery with an empty tool registry. |

Worker metrics are `liquid_worker_ready`, `liquid_worker_active`, `liquid_worker_completed`, and
`liquid_worker_failures`. The endpoint exposes no CAS bytes, action inputs, credentials, or
identity private material.
