<!--
DRAFT for https://github.com/oramasearch/liquid-bridge/issues/11. Not in force.

Before this policy takes effect, the owner:
  1. replaces every [[PLACEHOLDER: ...]] (the reporting address, the PGP key, and every response time);
  2. answers every "Question for the owner" block and deletes the block;
  3. turns on "Private vulnerability reporting" under Settings -> Code security;
  4. deletes this comment and the "Draft" notice below.
The pre-public checklist that tracks these steps is liquid-bridge docs/security/pre-public-checklist.md.
-->

# Security policy

> **Draft, not in force.** The reporting address, the response times and the safe-harbour wording
> below are waiting on the owner's approval. Until then, report through GitHub private vulnerability
> reporting only.

liquid is a version control system, build system, package manager, artifact service and runtime in
one program. It opens repositories it did not create, runs build tools it does not trust inside a
sandbox, speaks to remote caches and to Lake over the network, signs every write with the user's
key, and encrypts content with keys it holds. We treat every report about it seriously.

## Reporting a vulnerability

**Do not open a public issue, pull request or discussion for a security problem.**

Report it privately:

1. **GitHub private vulnerability reporting** (preferred): *Report a vulnerability* on this
   repository's *Security* tab. Only the maintainers can see the advisory it creates.
2. **Email:** [[PLACEHOLDER: REPORTING_ADDRESS]]. To encrypt, use the PGP key
   [[PLACEHOLDER: PGP_FINGERPRINT]], published at [[PLACEHOLDER: PGP_KEY_URL]].

Include what you can of:

- the version (`liquid version`) or the commit;
- the platform (Linux, macOS or Windows) and what was involved: the daemon, the sandbox or
  `liquid wrap`, a `liquid serve` command, a worker, sync, a hardware key, or a file format;
- what the problem is and what an attacker gains;
- how to reproduce it: a repository, an input file or a command line, and any crash output
  (sanitizer reports are especially welcome);
- whether it is already public, and whether an automated tool found it.

A person, not an autoresponder, acknowledges every report.

## What you can expect from us

| Step | Target |
|---|---|
| Acknowledge the report | [[PLACEHOLDER: ACK_TARGET]] |
| First assessment: confirmed or declined, with a severity | [[PLACEHOLDER: TRIAGE_TARGET]] |
| Status updates while the report is open | [[PLACEHOLDER: UPDATE_INTERVAL]] |
| Fix released, **critical** (code execution, sandbox escape, signature or verification bypass, key disclosure) | [[PLACEHOLDER: CRITICAL_FIX_TARGET]] |
| Fix released, **high** | [[PLACEHOLDER: HIGH_FIX_TARGET]] |
| Fix released, **medium** | [[PLACEHOLDER: MEDIUM_FIX_TARGET]] |
| Fix released, **low** | [[PLACEHOLDER: LOW_FIX_TARGET]] |

> **Question for the owner (response times):** the September draft proposed 3 business days to
> acknowledge, 7 days to assess, updates every 14 days, and fixes in 14 / 30 / 90 days by severity,
> low in the next release. Keep those, change them, or publish the policy without numbers? Each
> number here is a public commitment.

These are targets, not guarantees. If we are going to miss one, we tell you why and give a new date.
When a fix ships we publish a GitHub security advisory, request a CVE through GitHub, credit you in
the advisory unless you prefer not to be named, and list the fix in the
[release notes](docs/releases/README.md).

## Coordinated disclosure

We ask that you keep the details private until a fix is released or
[[PLACEHOLDER: DISCLOSURE_WINDOW, proposed 90 days]] have passed since your report, whichever comes
first. If a fix needs longer, for example because it changes a wire or storage format that other
software reads, we will ask for an extension and say why. If we stop answering and do not reply to a
reminder, you may publish once that window has passed.

If the problem is in a third-party component we build with (see *Scope*), we coordinate with its
maintainers and may ask you to report to them too.

We do not run a bug bounty.

## Safe harbour

> **Question for the owner (legal wording, needs approval and possibly counsel):** the text below is
> the September draft's proposed safe harbour. It promises not to pursue legal action under
> anti-hacking and anti-circumvention laws or the licence, and to say publicly that compliant
> research was authorised. Approve it as written, replace it with a standard text (for example the
> disclose.io core terms), or narrow it? It names U.S. and U.K. statutes; the company's own
> jurisdiction may call for different ones.

*Proposed text, not in force:*

We consider security research done in good faith and in line with this policy to be authorised. For
such research we will not pursue or support legal action against you under anti-hacking laws (such
as the U.S. Computer Fraud and Abuse Act or the U.K. Computer Misuse Act), anti-circumvention laws
(such as DMCA §1201) or this repository's licence terms, and we will not report you to law
enforcement. If a third party takes legal action against you for research that followed this policy,
we will make it known that your research was authorised.

Good faith means that you:

- test only your own installations, checkouts, stores, daemons and servers, or test instances you are
  allowed to use, and never another person's data, keys or repositories without their consent;
- run no denial-of-service or volumetric tests against any system you do not own, including the
  hosted service Lake, whose own policy governs it;
- use no social engineering, phishing or physical attacks against maintainers, contributors or users;
- access, keep or change only the data needed to show the problem, stop once you have shown it, and
  delete what you kept when the report is closed;
- report promptly, exploit the problem no further than the demonstration, and keep it private until
  the agreed date;
- follow the law that applies to you. This policy cannot authorise you to break a law we are not party
  to, and it does not bind anyone else.

If you are unsure whether something is covered, ask first through either reporting channel.

## Scope

**In scope:** this repository, `oramasearch/liquid`, and the releases built from it.

| Area | Where |
|---|---|
| The store: chunker, packs, index, canonical CBOR records, encryption at rest, garbage collection | `src/store/`, `src/encoding/`, `src/objects/`, `src/encryption/` |
| The daemon, its socket and its client protocol; change capture and the projected filesystem | `src/daemon.cpp`, `src/daemon/`, `src/capture/`, `src/vfs/` |
| The build executor, the sandbox, `liquid wrap` and its preload hook, the read guard | `src/build/`, `src/sandbox/` |
| Network surfaces: sync and `liquid serve`, REAPI over HTTP/2 and gRPC, the scheduler and workers, the admission server, the node store service, the HTTP/1.1 client and server | `src/sync/`, `src/serve/`, `src/http1/`, `src/http2/`, `src/protobuf/`, `src/reapi/`, `src/workers/`, `src/admission/`, `src/node_store/`, `src/net/` |
| Identity and signing: TAAP/1, key custody, hardware keys (FIDO2 and the other token layers), threshold signing, rotation | `src/identity.cpp`, `src/identity/`, `src/crypto/` |
| Verification: attestations and policy, provenance, X.509 and CMS, timestamps, TUF metadata, transparency logs | `src/attestation/`, `src/build/policy.cpp`, `src/build/verify.cpp`, `src/asn1.cpp`, `src/x509.cpp`, `src/cms.cpp`, `src/tuf.cpp`, `src/transparency.cpp`, `src/lake_transparency.cpp` |
| Secrets: sealed secrets, leaf encryption of JSON and `.env`, the secret scanner | `src/secret/`, `src/encryption/`, `src/scan/`, `src/formats/json.cpp`, `src/formats/dotenv.cpp` |
| Every parser of untrusted bytes: Git import and export, delta delivery, archive and package formats, SBOM inference | `src/git/`, `src/delta.cpp`, `src/formats/`, `src/build/sbom.cpp` |
| Distribution: installs, projection into packages and images, hot update | `src/install/`, `src/projection/`, `src/hot_update/`, `src/deploy.cpp` |
| The `liquid` command | `src/cli/` |
| The build itself: a way to make the CMake configure or the tests fetch or run something other than the pinned dependencies | `CMakeLists.txt`, `cmake/`, `.gitmodules` |

Opening, building and verifying **untrusted input is the core use, not an edge case**. A sandbox
escape, a write outside an action's declared outputs, a read the sandbox should have denied, a forged
or bypassed signature, a cached result served without verification, a daemon socket another local user
can drive, a leaked key or secret, unbounded memory or recursion on a malformed file, or a crash in any
parser are vulnerabilities to us.

**Out of scope**

- The hosted service **Lake** and its infrastructure. It has its own policy at
  [[PLACEHOLDER: LAKE_SECURITY_POLICY_URL]].
- The IDE, liquify, which has its own policy in
  [`oramasearch/liquify`](https://github.com/oramasearch/liquify/blob/dev/SECURITY.md).
- A problem only in a third-party dependency with no liquid-specific trigger (OpenSSL, zlib, zstd,
  BLAKE3, Monocypher, Boost, Capy, Corosio, automerge-cpp, tree-sitter). Report it upstream and tell us,
  so we can move to a fixed version.
- Problems that need the attacker to already have the user's operating-system account, root, or
  physical access, unless they cross a boundary liquid claims to enforce (the sandbox, the daemon's
  per-user socket, the signing key's custody).
- Missing hardening that is not itself exploitable, such as an absent compiler flag. Suggestions are
  welcome as ordinary issues.
- Automated scanner output with no analysis of exploitability.

**Known limits, documented and not vulnerabilities in themselves:** Windows has no build sandbox and no
daemon yet; the Linux read guard is a hermeticity aid and the namespaces are the enforcement; the secret
scanner is a control with false negatives, not a guarantee. The design documents below say where each
limit is written down.

## Supported versions

> **Question for the owner (support window):** proposed: before 1.0 only the latest tagged release is
> supported, and fixes ship in the next release cut from `dev`. Confirm or change.

| Version | Supported |
|---|---|
| Latest release (see [docs/releases](docs/releases/README.md)) | [[PLACEHOLDER: yes]] |
| Earlier releases | [[PLACEHOLDER: no]] |

## Design documents a report is judged against

Each component's promises, including what it must never do, are written down here:

- [`docs/library/store.md`](docs/library/store.md), [`encryption.md`](docs/library/encryption.md),
  [`leaf-encryption.md`](docs/library/leaf-encryption.md),
  [`sealed-secrets.md`](docs/library/sealed-secrets.md),
  [`secret-scanning.md`](docs/library/secret-scanning.md): the store, encryption and secrets
- [`docs/library/build-engine.md`](docs/library/build-engine.md), [`vfs.md`](docs/library/vfs.md),
  [`capture.md`](docs/library/capture.md),
  [`docs/build/sandboxed-executor.md`](docs/build/sandboxed-executor.md): the sandbox, the mount and
  capture
- [`docs/library/sync.md`](docs/library/sync.md), [`reapi.md`](docs/library/reapi.md),
  [`http2.md`](docs/library/http2.md), [`workers.md`](docs/library/workers.md),
  [`admission.md`](docs/library/admission.md), [`node-store.md`](docs/library/node-store.md): the
  network surfaces
- [`docs/library/identity.md`](docs/library/identity.md),
  [`attestations.md`](docs/library/attestations.md), [`provenance.md`](docs/library/provenance.md),
  [`policy.md`](docs/library/policy.md), [`tuf.md`](docs/library/tuf.md),
  [`transparency.md`](docs/library/transparency.md),
  [`release-approval.md`](docs/library/release-approval.md): signing and verification
- [`docs/library/hot-update.md`](docs/library/hot-update.md),
  [`distribution.md`](docs/library/distribution.md), [`projection.md`](docs/library/projection.md):
  distribution and updates
- [`docs/library/incremental-engine.md`](docs/library/incremental-engine.md): the daemon and its
  client protocol
- [`docs/standards/taap-1-spec-draft.md`](docs/standards/taap-1-spec-draft.md): the protocol and its
  threat model

Thank you for helping keep liquid safe for the people who trust it with their code.
