# Supply chain security and chain of custody

> Evidence: [`sources.md`](sources.md)
>
> **Requirement:** cryptographic chain of custody and signed builds, so that
> "viruses can't get in."

## The core claim

A defensible chain of custody is **not one signature**. It is an unbroken sequence
of verifiable claims linking a reviewed source commit to the bytes running in
production, where each link states *who* asserts *what*, and every link is
independently checkable.

A signature proves only "this identity vouched for these bytes." It does not prove
the bytes were built from the source you read, that the build was not tampered
with, or that the signer was not compromised. Those require separate mechanisms.

## The layers, and what each actually buys

**SLSA (v1.0/1.1)** defines build levels and, crucially, **provenance
attestations**: a signed statement describing what was built, from which sources,
by which builder, with which parameters. The value is that provenance is generated
*by the build platform*, not by the developer — so a compromised developer machine
cannot forge it.

What SLSA does **not** cover, and this is widely misunderstood: it says nothing
about whether the source is *good*. A perfectly SLSA-L3 build of malicious source
is still malicious. It defends the build, not the code.

**in-toto** is the general framework: a *layout* declares the expected steps of a
supply chain and who may perform each; *link* metadata records what actually
happened at each step; verification checks the actual chain against the declared
one. Attestations are wrapped in **DSSE** envelopes. Predicate types include SLSA
Provenance, SPDX, CycloneDX, and **VSA** (Verification Summary Attestation — "I
verified this, so you don't have to").

This is the model liquid should adopt structurally: **a policy stating what the
chain must look like, plus evidence of what it was, plus a checker.**

**Sigstore** solves key management, which is the practical reason signing fails in
the real world. `cosign` signs; **Fulcio** issues short-lived certificates bound to
an OIDC identity (a GitHub Actions workflow, a Google account); **Rekor** is an
append-only transparency log that records the signature so it cannot be quietly
issued and withdrawn. Keyless signing means there is no long-lived private key to
steal — the certificate is valid for minutes and the log proves it existed.

The criticisms are worth noting: keyless signing makes you dependent on an OIDC
provider and on Rekor's availability; Rekor's log grows enormously and monitoring
it is nontrivial; and "the workflow that signed this" is only as trustworthy as the
CI configuration, which is itself mutable.

**TUF** solves the *repository* problem that signing alone cannot: role separation
(root, targets, snapshot, timestamp), threshold signing, and explicit recovery
from key compromise. It defends against attacks signatures miss entirely — freeze
attacks (serving a stale index so clients never see a security update), rollback,
and mix-and-match (serving a valid but inconsistent set of packages). **Uptane**
extends it to vehicles.

**The Go module ecosystem is the best deployed example of these ideas working.**
`sum.golang.org` is a transparent checksum database; the module proxy serves
immutable versions; the client verifies every download against a signed,
append-only log. It is simple, mandatory-by-default, and it has largely eliminated
a class of attack. It is the model for what "good" looks like operationally.

## Reproducible builds: the strongest defence

If two independent parties build the same source and get **byte-identical**
output, then a compromised build machine cannot inject anything undetected. This
is qualitatively stronger than provenance — it does not require trusting the
builder at all.

Common sources of non-determinism: timestamps; locale and collation order; build
paths embedded in debug info; parallel emission ordering; ASLR-dependent output;
profile-guided optimisation; embedded UUIDs and build IDs. `SOURCE_DATE_EPOCH` is
the standard timestamp fix. **rebuilderd** and independent rebuilder networks
provide the verification side.

Reproducibility is also the mechanism that makes content-addressed outputs safe
([09](../09-package-managers/)) and shared build caches trustworthy
([06](../06-build-and-compiler-caching/)). It is not a separate feature — it is
load-bearing for three other systems.

## The attacks to design against

| Attack | Mechanism | What would have stopped it |
|---|---|---|
| **SolarWinds / SUNSPOT** | Malicious code injected into the *build process*, replacing source during compilation. Source repo and released binary diverged. | Reproducible builds; independent rebuilders comparing output. |
| **xz-utils / CVE-2024-3094** | `build-to-host.m4` present in the release tarball, **absent from Git**; decoded a payload from binary "test fixtures" at configure time. | No tarball/repo divergence; build from reviewed source only; treat build machinery and binary fixtures as code. See [05](../05-autotools-and-configure-era/). |
| **event-stream, ua-parser-js** | Maintainer account or trust transfer compromised; malicious version published. | Threshold signing; multi-party release approval; TUF-style role separation. |
| **Codecov bash uploader** | A served script was modified; consumers piped it to a shell. | Pinning by digest; never executing unpinned remote content. |
| **Dependency confusion / typosquatting** | A public package shadows a private name, or a near-miss name is installed. | Explicit registry scoping; lockfiles with digests; no implicit name resolution. |
| **3CX** | Cascading compromise — a compromised dependency's build compromised the dependent's build. | Hermetic builds; provenance verification of *inputs*, not just outputs. |

The pattern across all of these: **the build system is the target**, because it is
the point where trusted-looking output is produced from inputs nobody reviews as
carefully as source.

## Commit signing and its limits

Git commit signing proves an identity created a commit object. It does not prove
the content is good, that the signer's key was not stolen, or that the commit was
reviewed. Gitsign brings Sigstore's keyless model to commits.

Git's **SHA-1 → SHA-256 transition** remains incomplete years on. SHA-1 is broken
for collision resistance (SHAttered, and chosen-prefix attacks since), and while
Git's collision detection mitigates known attacks, an object model built on a
broken hash is a liability. **liquid has the enormous advantage of starting
fresh** and must not repeat this: pick a modern hash and make the format
agile from day one.

## SBOM: necessary, not sufficient

SPDX 3.0 and CycloneDX describe what is in an artifact; VEX describes whether a
known vulnerability actually applies. The gap everyone hits: an SBOM is a *claim*.
Unless it is generated by the build system that actually did the work — and signed
— it is a document someone wrote. Generating it from a build graph that already
knows every input is both more accurate and free.

## Implications for liquid

1. **Provenance is a byproduct, not a feature.** liquid already records, for every
   action, the complete input Merkle tree, the command, the environment, the
   toolchain and the outputs ([04](../04-build-systems/),
   [07](../07-distributed-execution/)). That *is* SLSA provenance. Emit it as a
   signed in-toto attestation automatically for every action, with no user
   configuration. Nothing else on the market gets this for free, because nothing
   else owns the whole chain.

2. **Reproducibility is a hard requirement, enforced by the tool.** Not a goal, not
   a badge. Sandboxed execution with a fixed environment, virtualised paths, no
   clock, and normalised outputs by construction
   ([06](../06-build-and-compiler-caching/)). Then *verify it continuously*:
   re-execute sampled actions on different workers and compare. A divergence is a
   critical alert. This single mechanism simultaneously validates the cache,
   detects compromised workers, and produces the evidence for L3-equivalent
   provenance.

3. **Modern hash, agile format, from commit zero.** BLAKE3 or SHA-256, with the
   digest algorithm encoded in every reference so it can be migrated. Git's SHA-1
   migration is a decade-long cautionary tale about the cost of getting this wrong.

4. **Sigstore-style keyless signing as the default, with keys as an option.**
   Short-lived certificates bound to an OIDC identity, recorded in a transparency
   log. Most developers will never manage a signing key correctly; do not require
   them to. Support long-lived keys, hardware tokens and threshold signing for
   organisations that need them.

5. **A transparency log for the artifact service.** The Go checksum database is the
   proof this works: an append-only Merkle log of every published artifact, so a
   client can verify it is seeing the same artifact everyone else sees, and a
   maintainer can detect an unauthorised publication. This directly serves the
   artifact-service requirement ([12](../12-artifact-distribution/)).

6. **TUF role separation for the repository layer.** Signing artifacts is not
   enough; the *index* must be protected against freeze, rollback and
   mix-and-match. Threshold signing and explicit key-compromise recovery.

7. **Verify inputs, not just sign outputs.** The 3CX and xz lessons. An action
   should be able to *require* that its inputs carry acceptable provenance, and
   refuse to build otherwise. Policy — "every dependency must have provenance from
   an approved builder" — should be expressible and enforced at build time, not
   discovered in an audit.

8. **No representable divergence between source and release.** The xz attack is
   structurally impossible if there is no separate tarball artifact — if what is
   published is derived, reproducibly and verifiably, from what is committed. Make
   this a property of the design rather than a policy people can bypass.

9. **Signed cache entries; least privilege on writes.** A shared build cache is a
   fleet-wide code execution primitive if anyone can write to it. CI and verified
   reproducible builds write; developers read.
