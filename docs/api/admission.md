# Admission server API

The admission listener serves the `/admit` routes; the control listener serves the remaining
routes. Request and response bodies are JSON except for the server-sent decision stream. The
admission listener is HTTPS unless the process was explicitly started with `--insecure-http`.

| Method | Route | Contract |
|---|---|---|
| `POST` | `/admit/<policy>` | Kubernetes `AdmissionReview` v1 containing a Pod; evaluates the named policy and returns every structured per-image decision in `response.status.decisions`. |
| `POST` | `/admission/review` | Compatibility route for the `default` set loaded by `--policy`. |
| `GET` | `/healthz` | Process liveness. |
| `GET` | `/readyz` | 200 only after all policy sets load and every enforcing attestation source answers its startup probe. |
| `GET` | `/metrics` | Prometheus counters and review-duration histogram. |
| `GET` | `/v1` | Version and capability discovery. |
| `GET` | `/v1/policies` | Loaded names and the last rejected-reload error, or null after valid input is active. |
| `GET` | `/v1/policy?name=<policy>` | Effective named policy after flag overrides; name defaults to `default`. |
| `POST` | `/v1/policy/reload` | Atomically reload the configured file or directory; invalid input returns 400 and leaves the previous sets active. |
| `GET` | `/v1/verify?policy=<name>&image=<ref@sha256:...>` | The shared image decision plus every considered attestation, issuer, profile, rule id, and verdict. |
| `GET` | `/v1/decisions?after=<sequence>&limit=<count>` | Server-sent structured decisions; retained replay precedes live records, and optional `limit` closes the response. |
| `GET` | `/v1/openapi.json` | OpenAPI 3.1 route document. |
| `POST` | `/mcp` | MCP initialize, `tools/list`, and `tools/call` for `admission_policies`, `admission_policy`, `admission_verify`, and `admission_decisions`. |

Metrics are `liquid_admission_reviews_total{decision,reason}`,
`liquid_admission_images_total{decision}`, and the `liquid_admission_review_seconds` histogram.
`GET /v1/verify` and the webhook call the same `review_image` operation and therefore cannot apply
different policy or verification rules. Verification selects `default` when `policy` is omitted.
For compatibility, verification also includes the previous `allowed`, `reason`, `attestation`, and
`considered` aliases beside the versioned record fields.

An allowed one-image response has this shape:

```json
{
  "apiVersion": "admission.k8s.io/v1",
  "kind": "AdmissionReview",
  "response": {
    "uid": "request-id",
    "allowed": true,
    "status": {
      "code": 200,
      "message": "admitted",
      "decisions": [
        {
          "v": 1,
          "kind": "admission-decision",
          "sequence": 1,
          "policy": "default",
          "subject": "sha256:...",
          "image": "registry/app@sha256:...",
          "outcome": "allowed",
          "reasons": [
            {"rule_id": "attestation.verified", "message": "attestation accepted"}
          ],
          "evidence": [
            {
              "attestation": "blake3:...",
              "profile": "image_provenance/1",
              "issuer": "taap:1:ed25519:builder",
              "verified": true,
              "rule_id": "attestation.verified",
              "reason": "attestation accepted",
              "log": {
                "outcome": "included",
                "checkpoint": "sha256:...",
                "checkpoint_sequence": 7,
                "tree_size": 1024,
                "reason": "included in verified log"
              }
            }
          ]
        }
      ]
    }
  }
}
```

A denied response changes `allowed` to false and status code to 403. Its message identifies the
first denied image, while `decisions` still contains the record for every image evaluated.
`evidence[].log` is null when log verification was not requested. Otherwise its `outcome` is
`included`, `not_included`, `unavailable`, or `denied`; failure reasons preserve the shared
verifier's tile coordinate where applicable. OpenAPI describes policy log mode and trust with
`AdmissionLogPolicy` and `LogTrust`; `AdmissionDecision`, `Evidence`, and `LogVerification`
describe the same record returned by `admission_verify` and retained by `admission_decisions` over
MCP.

`/v1/decisions` uses `id: <sequence>`, `event: decision`, and one or more `data:` lines containing
the decision JSON. The in-memory replay bound defaults to 256. Sequence numbers and retained
records reset when the admission process restarts.
