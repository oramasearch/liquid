# Image admission

`liquid serve admission` is the attestation-verifying Kubernetes admission webhook. It uses
libliquid's TAAP verifier directly. A Pod is admitted only when every image in `containers`,
`initContainers`, and `ephemeralContainers` is pinned by SHA-256 and has at least one completely
verified attestation from an accepted issuer under an accepted profile. In `attestation+log` mode,
that attestation must also have a positive transparency inclusion verdict.

## Named policy sets

`--policies <directory>` loads every regular `<name>.json` file as policy set `<name>`. Each set is
served at `POST /admit/<name>`. Non-JSON directory entries are ignored, and the directory must
contain at least one policy. The existing `--policy <file>` form is a singleton directory in
effect: it names the policy `default`, keeps `POST /admission/review` as a compatibility route, and
also serves `POST /admit/default`.

Each file uses this strict versioned JSON form:

```json
{
  "v": 1,
  "mode": "attestation",
  "issuers": ["taap:1:ed25519:example"],
  "profiles": ["image_provenance/1"],
  "exempt": ["registry.example/bootstrap/*"],
  "exempt_namespaces": ["kube-system"],
  "attestation_source": "/var/lib/liquid-admission/store",
  "log": {
    "lake": "https://lake.example",
    "max_checkpoint_age_seconds": 300
  }
}
```

`mode` is `off`, `attestation`, or `attestation+log`. `off` is the explicit audit-only policy: it
admits but still emits decisions. Exempt image patterns use `*` and `?` globs. An exempt namespace
permits the whole Pod; otherwise every image must verify or match an exemption. `--store` or
`--attestation-source` overrides the source in every set, with command-line flags taking precedence
over the documented environment variables.

The server reads policy files through `runtime::offload` and checks their contents from a Capy
reload coroutine. A changed directory is prepared in full and swapped as one map only after every
JSON file validates. An unreadable or invalid file leaves the complete previous map active. The
failure is printed to standard error, exposed as `last_reload_error` by `GET /v1/policies`, and
returned as HTTP 400 by `POST /v1/policy/reload`. Fixing the input clears the failure on the next
automatic reload. Deleting a valid file removes that named set in the same atomic swap.

`/readyz` requires at least one loaded set and a successful source probe for every enforcing set.
Audit-only sets need no source probe. `/healthz` reports process health separately.

## Image statement decision

An image is attested when an attestation subject equals the `sha256:...` digest in its digest-pinned
reference, its profile occurs in `profiles`, its TAAP issuer occurs in `issuers`, and
`verify_attestation` accepts every signature, profile, subject-kind, schema, freshness, status, and
disclosure rule. `image_provenance/1` gives the image projection work one exact statement:

- subject: the image's SHA-256 digest; subject kind: `artifact`
- `image:text`: the complete digest-pinned image reference
- `tree:text`: the `io.liquid.tree` digest
- optional `result:text`: the build result projected into the image

`liquid attest image <ref@sha256:...> --tree <digest> [--result <id>]` signs that record with the
device identity and writes it to the current repository store or `--store <path>`.

## Evidence sources and transparency

The internal `attestation_source` contract is one coroutine operation,
`by_subject(digest)`. The local implementation reads `store::attestations(subject)`. The network
implementation calls a node store at `GET /v1/attestations?subject=<digest>` and expects a JSON
`attestations` array containing hex-encoded canonical attestation records. This is not a sync route.

The `log_verifier` contract returns `included`, `not_included`, or `unavailable`. Its current lake
implementation reads `GET /v1/transparency_checkpoint` and writes the last response to
`--log-cache`. Complete tile/inclusion verification belongs to #83, so the implementation always
returns `unavailable` until that work lands. Consequently `attestation+log` currently refuses with
`log verifier unavailable`. Offline operation reads the cache and its configured age but refuses
anything it cannot prove; it never treats cached presence as inclusion.

## Decision record and stream

Each image in a Pod produces one version-1 `admission-decision` record. The public
`encode_admission_decision_cbor` / `decode_admission_decision_cbor` and JSON codec pairs use the
same fields:

```json
{
  "v": 1,
  "kind": "admission-decision",
  "sequence": 42,
  "policy": "strict",
  "subject": "sha256:...",
  "image": "registry.example/app@sha256:...",
  "outcome": "denied",
  "reasons": [{"rule_id": "attestation.missing", "message": "no attestation"}],
  "evidence": []
}
```

`subject` is null only when the request supplied no valid SHA-256 image digest. Evidence entries
name the attestation digest, profile, issuer, verification result, rule id, and reason for every
attestation considered. The AdmissionReview response always places all per-image records in
`response.status.decisions`; status code 200 means every record allowed the Pod and 403 means at
least one denied it.

The stable rule ids are `policy.audit_only`, `policy.image_exempt`,
`policy.namespace_exempt`, `image.digest_required`, `attestation.source_unavailable`,
`attestation.missing`, `attestation.subject_mismatch`, `attestation.issuer_rejected`,
`attestation.profile_rejected`, `attestation.signature_invalid`, `attestation.verified`,
`transparency.verifier_unavailable`, and `transparency.not_included`. The stable human reason
vocabulary remains `no attestation`, `issuer not accepted`, `profile not accepted`,
`signature invalid`, `image not pinned by digest`, `not in log`, `checkpoint stale`,
`log verifier unavailable`, `attestation source unavailable`, and `exempt`.

The control listener publishes the same records at `GET /v1/decisions` as server-sent events.
Event ids are process-local increasing sequence numbers. `?after=<sequence>` replays newer retained
records before following live decisions. The buffer keeps 256 records by default and
`--decision-replay <count>` changes that bound. A consumer that asks before the oldest retained id
observes the gap in the first returned id. `?limit=<positive count>` closes after that many records;
without it, the response follows decisions until the client disconnects. The buffer is local and
is not persisted.

The server applies the selected policy exactly as written. Kubernetes should use
`failurePolicy: Fail`; liquid never changes a denial into an admission after an internal or
transport failure. A namespace that needs leniency maps to an explicit audit-only policy set.

## Execution and transport

Webhook reviews, automatic reload, and decision streaming are Capy tasks dispatched on the listener
executor. Local disk work uses the runtime blocking adapter, and node-store/lake reads share
liquid's private Corosio HTTP client. The client request entry point is available in static and
shared builds so process-level admission tests exercise the same transport as the service.

The default listener is HTTPS on `0.0.0.0:8443`; each accepted socket is wrapped in Corosio's
`openssl_stream` before the vendored HTTP/1.1 parser reads it. `--insecure-http` is test-only and
prints a warning. The independent control listener defaults to `127.0.0.1:8083`.

The k0s integration must point `attestation_source` at a node store implementing the subject route,
or mount a liquid store directory. A `liquid serve sync` double cannot satisfy this contract.

## Deferred work

The webhook configuration, CRD, serving-certificate issuance, event adapter, and deployment
ownership remain in oramasearch/k0s#15. Image projection and automatic statement emission remain in
#28. Complete transparency verification remains in #83. Admission does not add runtime enforcement
after a Pod has started and does not change lake.
