# Supply chain and signing

> **Status:** hardware-held FIDO2 and PIV identity keys and signed `k`-of-`n` release approval are
> implemented by liquid #81. Keyless signing and TUF client metadata verification remain separate
> work.

> Research: [13-supply-chain-security](../research/13-supply-chain-security/) ·
> [05-autotools](../research/05-autotools-and-configure-era/)
>
> **Requirement:** cryptographic chain of custody and signed builds, so viruses
> cannot get in.
>
> **The mechanism is [TAAP/1](../standards/taap-1-spec-draft.md)** — every attestation
> below is a TAAP attestation, and every signer a TAAP identity. See
> [25](25-identity-attestation-and-transparency.md) for the model; this document is what
> liquid asserts with it.

## What a chain of custody actually is

Not one signature. An unbroken sequence of verifiable claims linking a reviewed
source change to the bytes running in production, where each link states *who*
asserts *what*, and every link is independently checkable.

A signature alone proves only "this identity vouched for these bytes." It does not
prove the bytes came from the source you read.

## Provenance is a byproduct

liquid already records, for every action: the complete input Merkle tree, the
command, the environment, the toolchain by content, the observed read manifest, and
the outputs ([04](04-build-engine.md), [05](05-caching.md)).

**That is SLSA provenance.** liquid emits it as a signed **TAAP attestation**
automatically, for every action, with no user configuration — carrying the SLSA
Provenance predicate as its `asserted_attributes` body under a registered
`attestation_profile`, so it stays interoperable with SLSA tooling while using one
attestation framework rather than two ([25](25-identity-attestation-and-transparency.md)). Nothing else on the
market gets this free, because nothing else owns the whole chain.

SBOMs come from the same place — generated from the build graph, complete and
correct by construction, rather than inferred from bytes by heuristic binary
scanning. Where liquid analyses a *foreign* artifact it falls back to binary
composition analysis, and **labels the difference clearly**: a derived SBOM and an
inferred one must never look alike to a consumer.

## Reproducibility is enforced

Not a goal or a badge — a mechanism other things depend on.

**Enforced by construction:** sandboxed execution, fixed environment, virtualised
paths, no clock, normalised outputs ([05](05-caching.md)).

**Verified continuously:** sampled actions re-execute in fresh local sandboxes in Phase 2, with
different-worker replay added in Phase 3. Exit status and every output digest are compared. A
divergence is a critical alert and permanently quarantines the cached result — it means a
non-hermetic action, a compromised worker, or a non-deterministic tool.

This one mechanism simultaneously validates the cache, detects compromised workers,
and produces the evidence for high-assurance provenance. It is why reproducibility
sits at the centre of the design rather than at the edge.

## Signing

**Keyless by default**, Sigstore-style: short-lived certificates bound to an OIDC
identity, recorded in a transparency log. Most developers will never manage a signing
key correctly, so do not require them to.

**Keys where they are wanted**: long-lived keys, hardware tokens (PIV, FIDO2), and
**threshold signing** for organisations that need multi-party release approval.

liquid owns the bounded CTAP2 and PIV command layers over operating-system HID and PC/SC
transports. It does not link libfido2 or OpenSC. This is liquid-bridge decision 21, decided by Steve
on 2026-09-05. The same transport interfaces accept protocol-level software doubles for CI.

Signed: changes, artifacts, cache entries, images, attestations and deployments —
by one signing layer, not five.

Image admission is implemented by `liquid serve admission`. Every Pod image is checked by digest
against the same TAAP registry and verifier, accepted issuer/profile policy, and optional
transparency verdict. The exact fail-closed flow and the `image_provenance/1` record are documented
in [`../library/admission.md`](../library/admission.md). Image projection and automatic emission
remain #28; complete transparency inclusion verification remains #83.

## Repository-level defence

Signing artifacts is not enough. TUF-style **role separation** (root, targets,
snapshot, timestamp) with threshold signing and explicit key-compromise recovery
defends against attacks signatures miss entirely: freeze attacks (serving a stale
index so clients never see a security update), rollback, and mix-and-match.

The implemented release layer is TUF-style independent signing: a signed `release_policy/1` names
the role, threshold, allowed TAAP identities, and expiry, while `release_approval/1` binds each
independent signature to the artifact and exact policy. See
[`../library/release-approval.md`](../library/release-approval.md). TUF metadata consumption remains
#186.

## Verifying inputs, not just signing outputs

An action can **require** that its inputs carry acceptable provenance and refuse to
build otherwise.

Policy — "every dependency must have provenance from an approved builder", "no
artifact without a reproducibility attestation from two independent builders" — is
expressible and enforced at build time, not discovered in an audit six months later.

The implemented `config.policy` form selects repository paths, the toolchain, or all inputs;
requires registered TAAP profiles from named identities or the repository's approved-builder list;
counts distinct issuers; and refuses or warns when evidence is missing. Its exact language,
fail-closed behavior, cache binding, and inspection commands are in
[`../library/policy.md`](../library/policy.md).

This is the lesson of 3CX and xz: the attacker targets your *inputs*.

## The attacks this structurally prevents

| Attack | Why it fails against liquid |
|---|---|
| **SolarWinds / SUNSPOT** — source and released binary diverge because the build machine was compromised | Independent reproduction and divergence detection catch it; a single builder is never trusted. |
| **xz-utils / CVE-2024-3094** — malicious `build-to-host.m4` in the release tarball, absent from git, decoding a payload from binary "test fixtures" | **There is no separate release tarball.** What is published is derived reproducibly and verifiably from what is committed, so source/release divergence is *unrepresentable*, not merely discouraged. Build machinery is recorded as actions with observed inputs, and an action feeding undecodable binary data to a compiler is detectable. |
| **Compromised maintainer account** | Threshold signing and multi-party release approval. |
| **Codecov-style modified remote script** | Everything is pinned by digest; nothing unpinned executes. |
| **Dependency confusion / typosquatting** | Explicit scoping and digest-pinned resolution ([07](07-dependency-resolution.md)); no implicit name resolution. |
| **Poisoned shared build cache** | Signed entries, read-only developer machines, CI as trust anchor, sampled divergence detection ([05](05-caching.md)). |

## Hash agility

BLAKE3 or SHA-256, with the algorithm identifier encoded in **every** reference.
Git's decade-long, still-incomplete SHA-1 → SHA-256 migration is the cautionary tale;
liquid starts fresh and must not repeat it.

Signature schemes are likewise pluggable, with post-quantum ML-DSA in view.

## What liquid does not claim

Provenance proves how something was built, not that it is *good*. A perfectly attested
build of malicious source is still malicious. liquid defends the build and the
distribution path; reviewing the code remains a human responsibility, and the tool
should not imply otherwise.

## Open questions

- **Dependence on an OIDC provider and a transparency log** for keyless signing, and
  what the offline/air-gapped story is.
- **How much divergence sampling is enough**, and who pays for it.
