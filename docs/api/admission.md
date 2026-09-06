# Admission server API

The admission listener serves `POST /admission/review`; the control listener serves the remaining
routes. Request and response bodies are JSON. The admission listener is HTTPS unless the process was
explicitly started with `--insecure-http`.

| Method | Route | Contract |
|---|---|---|
| `POST` | `/admission/review` | Kubernetes `AdmissionReview` v1 containing a Pod; returns the same version and request UID with the admission verdict. A denial carries status code 403 and `<image>: <reason>`. |
| `GET` | `/healthz` | Process liveness. |
| `GET` | `/readyz` | 200 only after policy load and a successful attestation-source startup probe. |
| `GET` | `/metrics` | Prometheus counters and review-duration histogram. |
| `GET` | `/v1` | Version and capability discovery. |
| `GET` | `/v1/policy` | Effective policy after flag overrides. |
| `POST` | `/v1/policy/reload` | Reload the configured policy file and source. |
| `GET` | `/v1/verify?image=<ref@sha256:...>` | The shared image decision plus every considered attestation, issuer, profile, and verdict. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 route document. |
| `POST` | `/mcp` | MCP initialize, `tools/list`, and `tools/call` for `admission_policy` and `admission_verify`. |

Metrics are `liquid_admission_reviews_total{decision,reason}`,
`liquid_admission_images_total{decision}`, and the `liquid_admission_review_seconds` histogram.
`GET /v1/verify` and the webhook call the same `review_image` operation and therefore cannot apply
different policy or verification rules.

An allowed response is:

```json
{"apiVersion":"admission.k8s.io/v1","kind":"AdmissionReview","response":{"uid":"request-id","allowed":true}}
```

A denied response additionally carries
`"status":{"code":403,"message":"registry/image:tag: image not pinned by digest"}`.
