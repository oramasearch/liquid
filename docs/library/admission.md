# Image admission

`liquid serve admission` is the attestation-verifying Kubernetes admission webhook. It uses
libliquid's TAAP verifier directly. A Pod is admitted only when every image in `containers`,
`initContainers`, and `ephemeralContainers` is pinned by SHA-256 and has at least one completely
verified attestation from an accepted issuer under an accepted profile. In `attestation+log` mode,
that attestation must also pass the shared checkpoint, parent-tile, and inclusion verifier against
the policy set's pinned log identity and witnesses.

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
  "mode": "attestation+log",
  "issuers": ["taap:1:ed25519:example"],
  "profiles": ["image_provenance/1"],
  "exempt": ["registry.example/bootstrap/*"],
  "exempt_namespaces": ["kube-system"],
  "attestation_source": "/var/lib/liquid-admission/store",
  "log": {
    "mode": "verify",
    "lake": "https://lake.example",
    "trust": {
      "identity": {
        "taap_id": "taap:1:ed25519:lake-production",
        "public_key": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
      },
      "witness_threshold": 2,
      "witnesses": [
        {
          "endpoint": "https://witness-1.example/checkpoint",
          "public_key": "123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0"
        },
        {
          "endpoint": "https://witness-2.example/checkpoint",
          "public_key": "23456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef01"
        }
      ]
    },
    "max_checkpoint_age_seconds": 300
  }
}
```

`mode` is `off`, `attestation`, or `attestation+log`. `off` is the explicit audit-only policy: it
admits but still emits decisions. Exempt image patterns use `*` and `?` globs. An exempt namespace
permits the whole Pod; otherwise every image must verify or match an exemption. `--store` or
`--attestation-source` overrides the source in every set, with command-line flags taking precedence
over the documented environment variables.

Within `log`, `mode` is `deny` or `verify`. `deny` is the explicit fail-closed selection: every
otherwise-valid attestation is refused with `transparency.policy_denied`. `verify` requires a
valid HTTP or HTTPS Lake URL, one Ed25519 log identity key, and a
witness threshold no larger than the configured witness list. Identity and witness keys are
32-byte lowercase hexadecimal values. The checkpoint signer must equal the configured TAAP id;
Lake's `/v1/identity` and `/v1/transparency_log_configuration` responses are not trust anchors for
admission.

`log.max_checkpoint_age_seconds` is optional. Omitting it disables freshness enforcement,
including for old checkpoints without an authenticated issuance time. When present it must be a
positive unsigned integer; zero is rejected when the policy loads. A rejected reload leaves the
previous complete policy map active. The public `admission_log_policy` represents omission as
`std::nullopt`, and encoding an omitted bound leaves the JSON member absent.

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

The `log_verifier` contract receives the exact canonical attestation bytes and returns an outcome,
checkpoint identity, checkpoint sequence, tree size, and reason. The Lake adapter uses
`transparency::http_log_source` and `transparency::log_client` directly. It verifies the checkpoint
signature and witness threshold against the policy trust, authenticates every full or partial
entry and parent tile through the selected checkpoint, locates the exact attestation bytes, and
verifies the checkpoint-bound inclusion proof. `--log-cache` records the canonical body of the
last successfully verified checkpoint for inspection; it is never sufficient for offline
admission, and a network failure remains `unavailable`.

Cryptographic, trust, malformed-response, tile, and proof failures are `not_included` with the
shared verifier's exact reason. Missing or forged tiles therefore retain their level, index, and
width in both the decision reason and the considered-evidence record. Transport failures are
`unavailable`. The adapter caches immutable tile bytes in memory, but the shared verifier decodes
and authenticates the complete path on every decision.

### Authenticated checkpoint age

[Bridge decision 37](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md)
adds `issued_at` to the `lake_checkpoint/1` body: a nonnegative integer number of seconds since
the Unix epoch, UTC. The field sits inside `checkpoint` in the HTTP response, alongside
`tree_size` and `root_hash`. It is covered by the existing Ed25519 signature over
`TAAP/1 lake_checkpoint/1`, one NUL byte, and the CBOR encoding of the complete body. The existing
canonical JSON body hash and witness signatures also cover it. No separate timestamp credential
is introduced. Lake publishes this field under lake#128; liquid consumes the decided format.

After signature, witness, tile, and inclusion verification, admission reads `issued_at` from the
authenticated body and compares it with its current UTC clock on every decision. With a positive
bound, missing or invalid time, age greater than the bound, or issuance more than **60 seconds**
in the future returns `not_included` with reason **`checkpoint stale`** and rule id
`transparency.not_included`. The record retains the verified checkpoint digest, sequence, and tree
size so the refusal identifies its evidence. Age exactly equal to the bound and future skew up to
and including 60 seconds are accepted. The skew allowance does not extend the maximum past age.

Fetched-at time and file modification time never establish checkpoint freshness. Replaying the
same signed checkpoint, reusing cached tiles, or reloading a stricter age bound cannot reset its
age. A stale checkpoint does not replace the inspection cache. Old checkpoints lacking
`issued_at` cannot satisfy any finite bound; this is the intended migration.

The log attests its own time. This protects against stale or replayed checkpoints, not against a
log that lies about when it signed. Binding RFC 3161 time from an independent authority is the
stronger option described in [transparency verification](transparency.md), and is outside this
admission change under decision 37.

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
```

`subject` is null only when the request supplied no valid SHA-256 image digest. Evidence entries
name the attestation digest, profile, issuer, verification result, rule id, reason, and nullable
log result for every attestation considered. Log outcomes are `included`, `not_included`,
`unavailable`, or `denied`. The AdmissionReview response always places all per-image records in
`response.status.decisions`; status code 200 means every record allowed the Pod and 403 means at
least one denied it.

The stable rule ids are `policy.audit_only`, `policy.image_exempt`,
`policy.namespace_exempt`, `image.digest_required`, `attestation.source_unavailable`,
`attestation.missing`, `attestation.subject_mismatch`, `attestation.issuer_rejected`,
`attestation.profile_rejected`, `attestation.signature_invalid`, `attestation.verified`,
`transparency.policy_denied`, `transparency.verifier_unavailable`, and
`transparency.not_included`. The stable human reason
vocabulary remains `no attestation`, `issuer not accepted`, `profile not accepted`,
`signature invalid`, `image not pinned by digest`, `not in log`, `checkpoint stale`,
`log verifier unavailable`, `log verification denied by policy`, `attestation source unavailable`,
and `exempt`; detailed verifier failures retain their original messages.

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

## Verification

The issue #290 regression lives in `tests/admission/test_log.cpp` and failed against the pre-fix
verifier at `7ff6ed82f93b916ef206c5776e2761b19536d8f3`: a checkpoint without authenticated time
returned `included in verified log` under a one-second bound. The fixed verifier returns
`checkpoint stale`. Run the focused checks with:

```sh
build/tests/liquid-tests --gtest_filter='AdmissionLogReview.*:AdmissionLogVerifier.*:AdmissionPolicy.*:AdmissionServer.enforces_checkpoint_age_and_stricter_reload_over_http'
```

The injected-clock cases cover exact age and skew boundaries, invalid time, unauthenticated time
changes, omitted bounds, integer limits, and replay with cached tiles. The process case mints a
real image attestation and serves a signed, zero-witness local HTTP log. `POST /admit/default`
allows the fresh checkpoint with a one-second bound, refuses the same checkpoint after expiry,
refuses excessive future skew, and applies stricter policy reloads to previously accepted evidence.
A zero-bound reload returns HTTP 400 and preserves the previous policy. Full native static and
shared builds run these checks through CTest as well.

## Deferred work

The webhook configuration, CRD, serving-certificate issuance, event adapter, and deployment
ownership remain in oramasearch/k0s#15. Image projection and automatic statement emission remain in
#28. Admission does not add runtime enforcement after a Pod has started and does not change lake.
