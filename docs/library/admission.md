# Image admission

`liquid serve admission` is the attestation-verifying Kubernetes admission webhook. It uses
libliquid's TAAP verifier directly. A Pod is admitted only when every image in `containers`,
`initContainers`, and `ephemeralContainers` is pinned by SHA-256 and has at least one completely
verified attestation from an accepted issuer under an accepted profile. In `attestation+log` mode,
that attestation must also have a positive transparency inclusion verdict. Kubernetes configures
the webhook with `failurePolicy: Fail`; transport or webhook failures never become permission.

## Policy

`--policy <file>` loads this strict versioned JSON form. `POST /v1/policy/reload` atomically replaces
the effective policy after the file validates and updates readiness from a new source probe.

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

`mode` is `off`, `attestation`, or `attestation+log`. Exempt image patterns use `*` and `?` globs.
An exempt namespace permits the whole Pod; otherwise every image must verify or match an exemption.
`--store` or `--attestation-source` overrides the policy source, with command-line flags taking
precedence over the documented environment variables. `/readyz` requires a loaded policy and an
attestation source that answered the startup probe. `/healthz` reports process health separately.

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

## Stable reasons

The decision and metric reason vocabulary is `no attestation`, `issuer not accepted`, `profile not
accepted`, `signature invalid`, `image not pinned by digest`, `not in log`, `checkpoint stale`,
`log verifier unavailable`, `attestation source unavailable`, and `exempt`. `checkpoint stale` is
reserved for #83's complete checkpoint proof. `mode: off` admits and records `exempt`, so disabled
enforcement remains visible without adding a second success vocabulary.

## Execution and transport

Webhook reviews are Capy tasks dispatched on the listener executor. Local disk work uses the
runtime blocking adapter, and node-store/lake reads share liquid's private Corosio HTTP client. The
client request entry point is available in static and shared builds so process-level admission tests
exercise the same transport as the service.
The default listener is HTTPS on `0.0.0.0:8443`; each accepted socket is wrapped in Corosio's
`openssl_stream` before the vendored HTTP/1.1 parser reads it. `--insecure-http` is test-only and
prints a warning. The independent control listener defaults to `127.0.0.1:8083`.

The k0s integration must point `attestation_source` at a node store implementing the subject route,
or mount a liquid store directory. A `liquid serve sync` double cannot satisfy this contract.

## Deferred work

The webhook configuration, CRD, serving-certificate issuance, and deployment ownership remain in
oramasearch/k0s#15. Image projection and automatic statement emission remain in #28. Complete
transparency verification remains in #83. Admission does not add runtime enforcement after a Pod
has started and does not change lake.
