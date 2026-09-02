<!-- Raw research source. Generated via Perplexity `perplexity_research` (web-grounded,
     multi-source) on 2026-09-01 for the liquid project. Lightly reformatted only.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# Software supply-chain security and cryptographic chain of custody, 2024–2026

## Executive assessment

A defensible chain of custody is not one signature. It is a sequence of independently verifiable bindings:

\[
\text{reviewed source digest}
\rightarrow
\text{declared build inputs}
\rightarrow
\text{isolated build execution}
\rightarrow
\text{artifact digest}
\rightarrow
\text{signed provenance/SBOM}
\rightarrow
\text{transparency log}
\rightarrow
\text{authorized release}
\rightarrow
\text{rollback-resistant distribution}
\]

The strongest practical architecture combines:

1. **SLSA/in-toto provenance** to say what process produced which digest.
2. **Sigstore or managed offline keys** to authenticate attestations.
3. **Transparency logs** to make signing events publicly detectable and difficult to equivocate about.
4. **Reproducible builds plus independent rebuilders** to verify that the binary really follows from the claimed source.
5. **TUF/Uptane** to authorize and safely distribute the currently intended artifact.
6. **SBOM plus VEX** for component and vulnerability information—not as substitutes for provenance or binary verification.

No one layer proves that software is benign. A valid signature can authenticate malicious software; provenance can faithfully describe a malicious build; an SBOM can accurately inventory a backdoored binary; and reproducibility can faithfully reproduce malicious source. Security comes from composing these mechanisms with policy, review, isolation, monitoring, and independent verification.

---

# 1. SLSA v1.0 and v1.1

SLSA v1.1 became the approved specification in April 2025 and replaced v1.0. It is backward-compatible: its principal changes are clarifications to the threat and attestation models, VSA verification, and verifier metadata—not a new level hierarchy. [web:377][web:378]

## Build levels

| SLSA build level | Required assurance | Technical meaning |
|---|---|---|
| **Build L0** | None | No provenance or build-integrity claim. |
| **Build L1** | Provenance exists | The build emits provenance identifying output artifacts by cryptographic digest and describing how they were produced. There is no mandatory provenance authenticity or accuracy guarantee. |
| **Build L2** | Authentic provenance from a hosted build platform | Consumers can authenticate the provenance, normally by verifying a digital signature associated with the hosted builder. Provenance must be produced by the build platform’s control plane rather than simply asserted by tenant build code, although some fields have permitted tenant-generation exceptions. |
| **Build L3** | Unforgeable provenance and an isolated build platform | Tenant-controlled build steps cannot access the attestation key or inject provenance fields; all external parameters are enumerated; concurrent and successive builds cannot influence one another; build environments are ephemeral; and shared-cache poisoning must not change outputs. |

The normative v1.1 matrix is: provenance **exists** at L1–L3; provenance is **authentic** and the platform **hosted** at L2–L3; provenance is **unforgeable** and builds **isolated** at L3. [web:144] The detailed L3 requirement places signing secrets in a secure management system accessible only to the build service account—not to user build steps—and requires trusted control-plane generation or verification of every provenance field. [web:137]

### Provenance structure

SLSA Provenance v1 is normally placed in an in-toto Statement whose `subject` contains artifact names and digest sets. Its predicate has:

```json
{
  "buildDefinition": {
    "buildType": "URI identifying build schema",
    "externalParameters": {},
    "internalParameters": {},
    "resolvedDependencies": [
      {"uri": "...", "digest": {"sha256": "..."}}
    ]
  },
  "runDetails": {
    "builder": {
      "id": "URI",
      "builderDependencies": [],
      "version": {}
    },
    "metadata": {
      "invocationId": "...",
      "startedOn": "...",
      "finishedOn": "..."
    },
    "byproducts": []
  }
}
```

`externalParameters` are caller-controlled inputs, such as repository/ref or build flags; `internalParameters` are platform-set values not available to the caller; and `resolvedDependencies` records inputs actually resolved by the builder. `runDetails` identifies the trusted builder and the particular execution. [web:168][web:171]

A verifier should therefore check at least:

- The bytes being consumed hash to one of the Statement’s `subject.digest` values.
- The attestation signature chains to an accepted builder identity.
- `predicateType`, `buildType`, and builder ID are recognized—not merely syntactically valid.
- Source and dependency digests, parameters, and expected build workflow satisfy policy.
- The builder was certified or otherwise assessed for the claimed SLSA level.
- The signing certificate and log inclusion correspond to the time and identity expected by policy.

### Isolated is not hermetic

| Property | Isolated build | Hermetic build |
|---|---|---|
| Primary purpose | Prevent cross-build, host, secret, persistent-state, and cache influence | Make every influencing input explicit and locally supplied |
| Network | May use network or remote execution if intentionally requested and represented appropriately | Normally no undeclared network access; dependencies are pre-fetched and content-pinned |
| Fresh environment | Required by SLSA L3 isolation | Normally required in a sound implementation |
| SLSA v1.0/v1.1 requirement | **Required at L3** | **Not required at any Build level** |
| Reproducibility implied? | No | No—hermetic builds may still embed time, paths, random values, or unstable ordering |

SLSA explicitly says L3 isolation is not hermeticity and does not prohibit calls to remote services or self-hosted runners outside the builder’s trust boundary. Hermeticity remains a future-direction property rather than an L3 requirement. [web:137]

## What SLSA v1.0/v1.1 does not cover

| Non-guarantee | Consequence |
|---|---|
| **No proof that source is safe or reviewed** | A perfectly attested build can compile a deliberately inserted backdoor. v1.0 removed the earlier source requirements; v1.1 has only the Build track. [web:138][web:388] |
| **No transitive dependency assurance** | A package’s level says nothing about the SLSA levels of its dependencies. [web:139] |
| **No vulnerability, quality, or runtime-behavior proof** | Provenance says how bytes arose, not whether they contain exploitable or malicious behavior. |
| **No required hermeticity or reproducibility** | An L3 build can intentionally download an unpinned object and can be nondeterministic. |
| **No absolute builder-operator defense** | L3 is mainly designed to stop a tenant or compromised build step from forging trusted provenance. A fully compromised platform control plane, hypervisor, signing service, or authorized operator remains in the trusted computing base. |
| **No completeness guarantee for all dependencies** | Even at higher levels, completeness of `resolvedDependencies` is best effort. [web:137] |
| **No artifact-distribution freshness or rollback protection** | SLSA does not decide which signed version is current; TUF or equivalent update metadata is needed. |
| **No guarantee the artifact itself was separately signed** | The provenance signature authenticates the attestation. The subject digest binds it to an artifact, but that is distinct from an artifact/release signature. |
| **No protection from an intentionally unsafe build definition** | L3 ensures a well-intentioned build executes with isolation; it does not prohibit a producer from requesting an insecure remote input. [web:137] |

---

# 2. in-toto, attestations, DSSE, and predicates

## Classic in-toto chain-of-custody model

| Object | Purpose and authenticated content |
|---|---|
| **Layout** | Project-owner-signed policy describing expected steps, authorized functionary keys, thresholds, expected commands, artifact rules, inspections, and expiration. |
| **Link metadata** | Functionary-signed evidence that a step ran: command, materials/input path hashes, products/output path hashes, and byproducts such as return value or output. |
| **Artifact rules** | `MATCH`, `CREATE`, `DELETE`, `MODIFY`, `ALLOW`, `REQUIRE`, and `DISALLOW` constrain how hashed files may flow between steps. |
| **Verification** | Verify layout signature and expiration; require the threshold of authorized links for each step; verify link signatures; then evaluate material/product rules and inspections. |

`MATCH ... FROM <step>` cryptographically chains one step’s products to another step’s materials. A final `DISALLOW *` is important because unmatched artifacts are otherwise allowed by default. [web:391][web:392] The reference verification procedure checks layout signatures and expiry, link thresholds, functionary authorization, and conformity of materials and products with the layout. [web:396]

Classic in-toto is strong for a predeclared, multi-party workflow. The newer **Attestation Framework** generalizes signed claims so that every claim does not require one monolithic layout.

## Statement and DSSE envelope

An in-toto Statement v1 has:

```json
{
  "_type": "https://in-toto.io/Statement/v1",
  "subject": [
    {"name": "artifact.tar.gz",
     "digest": {"sha256": "…"}}
  ],
  "predicateType": "https://example/schema/v1",
  "predicate": {}
}
```

It is commonly wrapped in a DSSE JSON envelope:

```json
{
  "payloadType": "application/vnd.in-toto+json",
  "payload": "<base64 raw Statement bytes>",
  "signatures": [
    {"keyid": "...", "sig": "<base64 signature>"}
  ]
}
```

DSSE signs the following unambiguous pre-authentication encoding rather than requiring canonical JSON:

\[
\operatorname{PAE}(T,B)=
\text{"DSSEv1"}\|\text{SP}\|\operatorname{LEN}(T)\|\text{SP}\|T\|
\text{SP}\|\operatorname{LEN}(B)\|\text{SP}\|B
\]

The verifier base64-decodes the payload, reconstructs PAE from the raw bytes and `payloadType`, and verifies each signature over that PAE. Length prefixes and signing the type prevent parsing/type-confusion attacks, while signing raw bytes avoids a large canonicalization attack surface. [web:271][web:282] The in-toto envelope specification requires an array of signatures and requires `payloadType` to be authenticated with the payload. [web:276]

## Important predicate types

| Predicate | What it asserts | What it does not establish |
|---|---|---|
| **SLSA Provenance** | How one or more artifact digests were produced: builder, build type, parameters, resolved dependencies, timing and byproducts | That source or output is non-malicious |
| **SPDX** | An SPDX BOM associated with the Statement subjects | That the BOM was generated completely or from the final shipped bytes |
| **CycloneDX** | A CycloneDX BOM, dependency graph, services, vulnerabilities or related BOM information | Provenance or exploitability by itself |
| **VSA — Verification Summary Attestation** | A verifier evaluated subject artifacts and input attestations against a digest-bound policy and obtained a result/verified level | The result is trustworthy unless the verifier identity and policy are trusted |
| **Link predicate** | Migration/generalization of classic in-toto step link evidence | Full workflow conformance unless evaluated against policy |
| **Vulnerability/VEX-related predicates** | Scanner findings or context-specific affected/not-affected assertions | Source-to-binary correspondence |

The registered in-toto predicate set includes SLSA Provenance, Link, SPDX, CycloneDX, vulnerability reports, and SLSA Verification Summary. [web:121]

A VSA v1 identifies the `verifier`, verification time, resource URI, policy URI and digest, input attestations, `verificationResult`, `verifiedLevels`, dependency-level results, and SLSA version. This allows a central trusted verifier to compress a large attestation bundle into a signed policy decision, but shifts trust to that verifier and exact policy digest. [web:172]

---

# 3. Sigstore: Cosign, Fulcio, Rekor, Gitsign, and TUF

## Keyless signing protocol

“Keyless” means **no user-managed long-lived signing key**, not “no private key.”

1. Cosign or Gitsign creates an ephemeral public/private key pair.
2. The signer authenticates to an OIDC issuer such as GitHub Actions, Google, or another configured identity provider.
3. The client sends Fulcio the public key and OIDC token.
4. Fulcio verifies the token and binds its subject/workload claims to the certificate’s SAN/extensions.
5. Fulcio submits a precertificate to its certificate-transparency log and obtains an SCT.
6. Fulcio returns a short-lived certificate—commonly around ten minutes—covering the ephemeral public key.
7. The client signs the artifact digest or DSSE PAE with the ephemeral private key.
8. The certificate and signature are submitted to Rekor.
9. Rekor supplies signed time/inclusion evidence.
10. Verification checks artifact digest, signature, Fulcio chain, identity and issuer policy, certificate/log evidence, and Rekor inclusion.

Sigstore’s threat model describes the OIDC→Fulcio precertificate→CT log→certificate→artifact signature→Rekor flow, including SCT, SET, and inclusion checks. [web:151] Cosign’s verification interface requires policy constraints such as `--certificate-identity` and `--certificate-oidc-issuer`; merely accepting any valid Fulcio identity is unsafe. [web:434]

## Component roles

| Component | Security function |
|---|---|
| **Cosign** | Signs and verifies container images, blobs, attestations and bundles; supports key-based and identity/keyless operation. For OCI, signatures and attestations are normally associated with immutable image digests, not trusted mutable tags. |
| **Fulcio** | Short-lived certificate authority that converts successful OIDC authentication into an X.509 certificate binding an ephemeral key to a human or workload identity. |
| **Fulcio CT log** | Makes Fulcio-issued certificates visible and auditable; distinct from Rekor. |
| **Rekor** | Append-only transparency log for signatures, certificates and attestation-related entries. It supplies time and Merkle inclusion evidence and makes unauthorized signing discoverable. |
| **Gitsign** | Git signing helper using the Sigstore keyless flow for commits; signing details are recorded in Rekor. [web:42] |
| **Sigstore trust root** | Trusted Fulcio CA certificates, Rekor/log verification keys, CT-log keys and validity intervals. |
| **TUF repository** | Authenticates and updates the Sigstore trust root and enables key rotation/recovery without trusting an unauthenticated download. |

Sigstore distributes Fulcio roots and Rekor public keys through TUF; Cosign starts from an embedded TUF root and obtains updated verification material from the public-good-instance repository. [web:31][web:32]

### Offline verification

A modern bundle can carry the signature, Fulcio certificate, timestamps, Rekor entry, inclusion proof and signed checkpoint. The verifier still needs independently trusted Sigstore root material; it must not accept log or CA keys from the untrusted bundle itself. The inclusion audit path is folded into a root and compared with a log-signed checkpoint, allowing cryptographic verification without a live Rekor query. [web:421][web:430]

## Rekor cryptography

Rekor uses a binary Merkle tree. Conceptually:

\[
L_i = H(0x00 \parallel \text{entry}_i)
\]

\[
N = H(0x01 \parallel N_\text{left}\parallel N_\text{right})
\]

An inclusion proof contains roughly \(\log_2 n\) sibling hashes. A verifier combines the leaf with those siblings and checks that the result equals the root in a signed checkpoint. A consistency proof shows that a later tree is an append-only extension of an earlier tree rather than a rewritten history. Rekor v2 specifies logarithmic inclusion proofs over tree shards. [web:350] These constructions are tamper-evident, not magically tamper-proof: a malicious log can show different valid-looking roots to isolated users unless checkpoints are gossiped, witnessed or independently monitored.

## Criticisms and operational limits

| Criticism | Technical reality and mitigation |
|---|---|
| **OIDC becomes signing authority** | Compromise of an OIDC account gives the attacker signing power for resources that trust it; compromise of an issuer can affect the issuer’s whole identity domain. Require hardware-backed MFA, tightly scoped workflow identities, protected release environments and exact issuer/subject policy. |
| **Fulcio compromise permits false certificates** | Rekor/CT can make the event visible, but visibility does not stop immediate signing. TUF-mediated CA revocation and compromise-time handling are essential. |
| **Identity is not authorization** | A Fulcio-valid identity only proves that the signer authenticated as that identity at that time; it does not prove the identity should sign that project or that the artifact is good. [web:151] |
| **Short-lived certificates do not cure active account theft** | They reduce theft of dormant long-term keys, but an attacker controlling the account or CI workload can repeatedly request fresh certificates. |
| **Public logs leak metadata** | Human keyless certificates have historically exposed identity information such as email addresses in public logs. [web:153] |
| **A log does not prevent malicious publication** | It creates durable evidence and enables detection. Prevention still requires registry/release admission policy. |
| **Split-view/equivocation requires monitors** | Inclusion in a root supplied by the same log is insufficient unless the root is witnessed or compared. Sigstore says identity and consistency monitoring are critical and verifiers should consume monitor alerts. [web:151] |
| **Monitoring at ecosystem scale is harder than proof verification** | Inclusion proofs are \(O(\log n)\), but discovering every unexpected use of a workload identity may require indexing or processing the complete log stream. Rekor v1 lacked efficient SAN-oriented queries for some keyless workload identities; scanning its firehose was operationally expensive. Rekor v2’s bulk tiles substantially improve this. [web:163] |
| **Monitoring coverage is not automatic** | Sigstore documents only a small set of monitoring options; identity support in its monitor remains limited by entry type and identity matching. [web:158] |
| **Availability and historical recovery** | A Rekor outage can impede online checks unless proofs were bundled. A compromised log plus compromised monitors can destroy confidence in historical timestamps and may require re-signing. [web:151] |

---

# 4. TUF and Uptane

TUF answers a question signatures alone do not: **which keys are authorized for which files, and which signed version is current?**

## Top-level role separation

| Role | Signed content | Typical key posture | Security effect |
|---|---|---|---|
| **Root** | Root keys, keys and signature thresholds for all top-level roles, metadata version and expiry | Multiple offline keys; high threshold; infrequently used | Bootstraps trust, rotates/revokes role keys, limits single-key compromise |
| **Targets** | Authorized target paths, file lengths and cryptographic hashes; optional delegated target roles and path constraints | Offline or highly protected; may use delegation | Prevents arbitrary files from being accepted as releases |
| **Snapshot** | Version and/or digest information for all current targets and delegated-target metadata | Separate from timestamp; preferably offline or more protected | Presents one mutually consistent repository state; stops metadata mix-and-match |
| **Timestamp** | Current snapshot metadata version/hash/length | Online, frequently rotated, short expiry | Cheap freshness check; limits freeze attacks |

These contents and relationships are normative TUF concepts. Snapshot metadata prevents targets metadata from different repository states being combined, while timestamp is small, frequently re-signed and normally uses an online key. [web:28]

## Threshold signing and compromise recovery

If root requires \(t\) signatures from \(n\) keys, an attacker must compromise at least \(t\) authorized keys to issue trusted root metadata. Different roles should use different keys, so compromise of the online timestamp key does not authorize new targets.

Root rotation is unusually strict: root version \(N+1\) must meet the old root’s threshold **and** the new root’s threshold. This provides continuity from the client’s trusted root while proving control of the new key set; clients also require sequential root versions to prevent rollback or skipped malicious transitions. [web:16]

Recovery:

- A compromised timestamp, snapshot or targets key is replaced through newly signed root metadata.
- A non-threshold root compromise can likewise be repaired using the remaining root threshold.
- If an attacker obtains a threshold of root keys, ordinary in-band recovery is no longer trustworthy; a new root must be delivered through an external trusted channel. [web:17]
- Expirations and role separation constrain the compromise window, while target delegations reduce blast radius by namespace/path.

## Attack defenses

| Attack | TUF mechanism |
|---|---|
| **Arbitrary software substitution** | Client accepts only a target whose length and digest match threshold-signed targets metadata reached through the trusted role graph. |
| **Rollback** | Clients persist highest-seen metadata versions and reject lower versions; root must advance exactly one version at a time. |
| **Freeze** | All metadata expires; short-lived timestamp metadata forces periodic freshness. An attacker can deny service, but cannot indefinitely make stale metadata look current after expiry. |
| **Mix-and-match** | Snapshot pins the versions/hashes of the complete targets/delegation metadata set. |
| **Fast-forward/recovery obstruction** | Version and expiration validation prevent attackers from forcing arbitrary version transitions; sequential root update rules ensure every trust-root transition is checked. |
| **Endless-data/download abuse** | Signed expected lengths let clients bound downloads. |
| **Key compromise** | Role separation, threshold signatures, offline root/targets keys, delegation and in-band root-authorized rotation. |

TUF does not guarantee repository availability or that authorized maintainers selected safe software. It converts many compromises into detectable refusal/expiry instead of silent code execution.

## Uptane

Uptane adapts TUF for vehicles with many ECUs, intermittent networks, constrained secondary controllers, and the requirement that a valid image also be appropriate for a particular vehicle.

| Uptane element | Function |
|---|---|
| **Image repository** | Publishes images and TUF-style signed metadata; uses strongly protected/offline authorization for image authenticity. |
| **Director repository** | Uses vehicle inventory and manifests to issue vehicle-specific signed instructions identifying what each ECU should install. |
| **Primary ECU** | Downloads and verifies time, metadata and images, uploads manifests, and distributes updates to Secondary ECUs. |
| **Secondary ECU** | Verifies applicable metadata and its assigned image to the degree required by its implementation profile. |
| **Dual-repository check** | An image must be both authentic according to the Image repository and intended for the vehicle according to the Director. |

The current Uptane design separates the Image repository from a Director that signs installation instructions on demand. [web:331][web:332] It retains root, targets, snapshot and timestamp roles while addressing freeze, rollback, mix-and-match, slow-retrieval and intra-vehicle distribution attacks. [web:340]

---

# 5. Reproducible Builds and independent rebuilders

A reproducible build produces bit-for-bit identical output from the defined source, build instructions and build environment. Fractions are not directly comparable because projects count different package sets, architectures, test conditions and whether they rebuild specially controlled tests or the binaries actually shipped.

## Best-supported status figures

| Ecosystem | Reproducible fraction | Scope/caveat |
|---|---:|---|
| **Debian** | **Over 95%; approximately 96%** | Debian trixie/amd64 testing and controlled infrastructure. `reproduce.debian.net` is stricter because it attempts to recreate archive binaries using their original `.buildinfo`; Debian reported trixie/amd64 above 95% in May 2025. [web:406][web:408] |
| **Arch Linux** | **About 88.5–89%** | Whole-repository measurements around 2024–2025; Arch’s minimal container userland reached 100%, which must not be confused with the entire repository. [web:212][web:219][web:15] |
| **NixOS/nixpkgs** | **Up to 93% in 2024** | Large-scale bitwise package-rebuild study. Earlier measured revisions rose from 69% in 2017 to about 91% in April 2023. Rebuildability/build-environment recreation above 99% is a different metric. [web:196][web:198] |
| **Fedora** | **About 90%** | Infrastructure improvements reached approximately 90%; Fedora 44’s policy goal was at least 99%, but public project material still contained a `TBD%` final field rather than proof that 99% had been reached. [web:301][web:304] |

Debian moved further in May 2026 by blocking migration of newly uploaded packages into the next testing release when they fail reproduction or regress, subject to exceptions. [web:412][web:415]

## Common sources of nondeterminism

| Source | How it changes bytes | Typical control |
|---|---|---|
| **Wall-clock timestamps** | Archive mtimes, PE/ELF metadata, `__DATE__`, generated docs, gzip headers | `SOURCE_DATE_EPOCH`; deterministic archive modes; normalize mtimes |
| **Locale/time zone** | Sort order, translated messages, date rendering, decimal formatting | `LC_ALL=C`, `TZ=UTC`, clean environment |
| **Build paths** | Debug paths, `__FILE__`, RPATHs, generated manifests | Canonical build directory; GCC/Clang `-ffile-prefix-map`/`-fdebug-prefix-map`; Go `-trimpath` |
| **Directory/hash-map ordering** | Archive members, link order, generated source or manifests | Sort inputs explicitly; deterministic collections and archive tools |
| **Parallelism/races** | Completion order changes concatenation, registration or linker inputs | Deterministic scheduling/data reduction; fix races rather than permanently forcing `-j1` |
| **Randomness/UUIDs/seeds** | Random identifiers, randomized hash tables, generated keys | Fixed reproducible seeds where safe; never reuse deterministic cryptographic secrets |
| **ASLR and memory addresses** | Pointer ordering or leaked addresses affect generated output; uninitialized memory changes bytes | Do not derive output from addresses; initialize all memory; temporarily disabling ASLR is a diagnostic, not the preferred fix |
| **Build IDs** | IDs may include random/time/path-dependent data or hashes of already nondeterministic sections | Deterministic content-derived IDs after normalizing other sections |
| **PGO/profile data** | Hardware, scheduling, ASLR, workload timing and profile merge order alter optimization decisions | Publish and hash the exact profile; collect with deterministic workload/environment; deterministic profile merges |
| **Toolchain/CPU differences** | Compiler version, linker, libc, CPU feature auto-detection | Pin complete toolchain and target architecture; disable host-native probing |
| **Host metadata** | Username, hostname, UID/GID, umask, filesystem order, PID | Sanitized container/VM/chroot; fixed ownership and umask; sorted directory enumeration |

The Reproducible Builds guidance explicitly identifies ASLR, parallel races, locale, timezone, umask and timestamps as sources of variation. [web:5][web:7]

### `SOURCE_DATE_EPOCH`

`SOURCE_DATE_EPOCH` is an ASCII Unix timestamp, excluding leap seconds. It must be deterministic from source and should normally represent the last source modification, including packaging changes. Supporting tools substitute it for the current clock when embedding required timestamps. [web:241][web:242]

It is not a universal “make reproducible” switch: it does nothing about paths, toolchain drift, network-fetched dependencies, races, random values, PGO, or unordered files.

## `rebuilderd` and independent rebuilders

`rebuilderd` monitors a binary repository, obtains source and build metadata, rebuilds packages independently, compares output digests/bytes, and publishes signed or otherwise queryable results. Multiple organizations should rebuild the same package so that the original build farm and one verifier are not a shared point of trust. Arch has had independent instances operated by the distribution, Wolfpit, NYU and other research groups. [web:254]

The key distinction is:

- **Repeat the same build twice on the same compromised worker:** weak.
- **Rebuild under a separately administered environment from authenticated source and build metadata:** strong.
- **Obtain matching results from several diverse independent rebuilders:** stronger.
- **For compilers, use diverse double compilation:** addresses Ken Thompson-style “trusting trust” threats more directly. [web:372]

## Why it is the strongest defense against build-system compromise

Provenance can be signed by the compromised build system. A release signature can sign the compromised output. An SBOM can describe it. In contrast, an independent deterministic rebuild asks a falsifiable question:

\[
H(\text{independent build}(\text{declared inputs}))
\stackrel{?}{=}
H(\text{distributed artifact})
\]

A SUNSPOT-style injector present only on the vendor build server causes a mismatch. The Reproducible Builds project describes reproducibility as the only way to detect such build-time substitutions early. [web:368][web:369]

The limits are important:

- Rebuilding malicious source reproduces malicious output.
- Two rebuilders using the same compromised compiler/toolchain may agree.
- Undeclared network input can defeat the premise.
- A targeted compiler trigger may require diverse double compilation.
- Reproduction must cover the exact distributed bytes—not a nearby source package or unsigned unpacked contents.

---

# 6. Attacks the controls must defend against

| Attack | Compromise and payload path | Why ordinary signing failed / relevant defenses |
|---|---|---|
| **SolarWinds / SUNSPOT** | SUNSPOT ran in the Orion build environment, watched compilation, replaced a source file with a backdoored version, built SUNBURST, then restored the original. SolarWinds states its source repository was not changed. [web:76][web:87] | The malicious binary passed through the legitimate build and signing pipeline. Independent reproducible rebuilds would expose source-to-binary mismatch; isolated ephemeral builders, EDR, two-party release controls and provenance from a separately protected control plane reduce risk. |
| **xz/liblzma CVE-2024-3094** | Malicious xz 5.6.0/5.6.1 **release tarballs** contained `m4/build-to-host.m4`; the Git source lacked this triggering macro. It decoded `bad-3-corrupt_lzma2.xz`, then extracted stages from `good-large_compressed.lzma`, eventually injecting `liblzma_la-crc64-fast.o` into `liblzma.so` under selected Linux build conditions. [web:80][web:181] | A signed release tarball could still differ from the tag. Reproduce the tarball from the signed tag, inspect generated Autotools files and opaque test fixtures, pin source digests, and require provenance binding exact tarball and Git-tree digests. |
| **event-stream / flatmap-stream** | A new maintainer added `flatmap-stream`; version 0.1.1 carried obfuscated code targeted at the Copay wallet. [web:316][web:319] | Maintainer authorization was abused rather than cryptography broken. Require maintainer-change review, lockfile review, dependency provenance, publisher MFA, delayed publication and behavior scanning. |
| **ua-parser-js** | A compromised maintainer npm account published malicious versions 0.7.29, 0.8.0 and 1.0.0. [web:317][web:318] | Registry authentication authenticated the compromised account. Use phishing-resistant MFA, trusted publishing from protected CI, namespace/version policy, provenance and rapid revocation. |
| **Codecov Bash Uploader** | An HMAC key was extracted from a public Docker-image layer and used to modify the Bash Uploader in Google Cloud Storage; CI consumers fetched and executed it, leaking environment secrets. [web:286] | `curl | bash` delegated current code selection to a mutable endpoint. Pin a cryptographic digest, verify a signature/provenance bundle, eliminate secrets from build jobs, and treat downloaded CI tools as executable dependencies. |
| **PyPI/npm typosquatting** | Attacker registers a visually or typographically similar package and relies on human error or misleading search results. npm describes this threat and attempts registry-side detection. [web:267] | Signatures on the wrong package do not help. Use exact names, lockfiles, approved namespaces, repository allow-lists, dependency review and package-manager policy. |
| **Dependency confusion** | A build consults internal and public indexes; attacker publishes the same internal package name publicly, often with a higher version, causing the resolver to select it. [web:257][web:258] | Use private namespaces/scopes, bind each scope to exactly one registry, prohibit public fallback, pin versions and source repository, and verify integrity/provenance. |
| **3CX** | Mandiant found a supply-chain chain: a 3CX employee installed a compromised X_Trader package, attackers entered 3CX and compromised its software-production environment, and malicious 3CX Desktop App releases reached customers. [web:290][web:291] | Valid vendor delivery propagated the compromise. Defenses include workstation-to-production separation, isolated builders, reproducible releases, provenance admission and TUF-style release authorization. |
| **Shai-Hulud, npm, September 2025** | A genuine self-replicating npm worm compromised developer credentials/tokens, injected itself into packages the account could publish, added install-time execution, stole further npm/cloud credentials and propagated without a central C2. CISA reported more than 500 compromised packages. [web:62][web:63] | This turns one package execution into registry-wide publisher compromise. Eliminate long-lived npm tokens, use OIDC trusted publishing scoped to repository/workflow/environment, disable install scripts where possible, sandbox builds without cloud credentials, and alert on unexpected provenance identities or bulk publication. |
| **Shai-Hulud 2.0 and 2026 variants** | Later waves retained autonomous credential theft and package propagation; reports describe multiple campaigns through April 2026 and a “Mini Shai-Hulud” wave involving npm and some PyPI packages. [web:65][web:70] | Ecosystem response must be structural: short-lived audience-bound publishing credentials, mandatory provenance, registry anomaly controls, rapid token invalidation, and isolation so a dependency install cannot reach publisher credentials. |

The recurring lesson is that signing answers **“which credential approved these bytes?”** It does not answer whether the credential was stolen, the release process manipulated, the source reviewed, or the distributed binary derived from that source.

---

# 7. Git commits, tags, Gitsign, and the hash transition

## Commit and tag signatures

A signed Git commit authenticates the serialized commit object, including:

- root tree object ID,
- parent commit ID(s),
- author and committer identity/timestamps,
- commit message.

Because trees recursively hash filenames, modes, blobs and subtrees, the signature indirectly commits to the snapshot. Parent IDs bind the commit to a particular history.

A signed annotated tag authenticates the tag object—normally target object ID, target type, tag name, tagger and message—and is preferable for release authorization. Git supports GPG, SSH and S/MIME signing; `git commit -S`, `git tag -s`, `git tag -v`, and signature-displaying log commands are the basic interfaces. [web:227][web:236][web:240]

### Limits of Git signing

- A signature proves possession or authenticated access to a signing identity, not code review or safety.
- A stolen key or compromised OIDC account can sign malicious history.
- Unsigned commits can remain reachable beneath a signed merge or tag unless policy checks the complete reachable history.
- A signed commit does not prove that CI built it or that a registry package corresponds to it.
- A signed tag does not stop deletion, withholding or replacement unless consumers pin the object ID and use transparency/freshness mechanisms.
- Web UIs’ “Verified” badges reflect the hosting provider’s identity-binding and verification policy; they are not a complete project authorization policy.
- Force-push protections, mandatory review and release provenance remain necessary.

## Gitsign

Gitsign plugs into Git’s signing interface, creates a keyless Sigstore signature using an OIDC identity and records the signing event in Rekor. [web:42] It removes long-lived developer-key administration and adds transparency, but inherits Sigstore’s IdP, Fulcio, Rekor, monitoring and privacy assumptions. It also does not link a commit automatically to the artifact built from it; the SLSA provenance must carry the source digest.

## SHA-1 to SHA-256 status as of September 2026

- Git has supported SHA-256-format repositories since Git 2.29.
- SHA-1 remains broadly deployed and the default in existing ecosystems.
- The transition design allows one repository at a time and envisions SHA-1↔SHA-256 object-ID translation tables. [web:91][web:97]
- Interoperability work began landing in Git 2.45, but remained incomplete in 2026; estimates still described hundreds of patches and uncertainty over whether complete interoperation would block Git 3.0. [web:94][web:104]
- Major hosting support remained uneven, so SHA-256 repositories were not yet a universal cross-forge choice.

Git’s hardened SHA-1 use and object-structure checks reduce practical collision exploitation, but migration remains desirable. More importantly, object hashing is integrity linkage, not actor authentication: SHA-256 Git objects still need signatures, authorization and build provenance.

---

# 8. SPDX 3.0, CycloneDX, VEX, and the SBOM gap

## Formats

| Format | 2024–2026 characteristics | Best fit |
|---|---|---|
| **SPDX 3.0/3.0.1** | Core object model plus purpose-specific Software, Licensing, Security, Build, Dataset and AI profiles; SPDX 3.0 was released in 2024 and 3.0.1 followed in December 2024. [web:108][web:117] | Legal/licensing exchange, rich cross-domain graphs, policy and procurement |
| **CycloneDX 1.6/1.7** | BOM-centric full-stack model covering components, services, dependencies, vulnerabilities, VEX and cryptographic assets; 1.6 became ECMA-424 in 2024 and 1.7 was published in 2025. [web:106][web:117] | Operational application security, dependency automation, VEX and CBOM |
| **VEX** | A set of context-specific assertions such as affected, not affected, fixed or under investigation, normally with justification and product/vulnerability identifiers | Reducing false positives and prioritizing vulnerabilities in a particular product context |

VEX is not a vulnerability waiver. It should be signed, time-bounded, product-version-specific, and tied to evidence. CycloneDX defines VEX as focusing on whether a vulnerability is exploitable in a component’s actual context. [web:110]

## The verification gap

An SBOM is a claim about composition. It does not automatically prove:

1. It was generated from the final shipped artifact rather than source manifests.
2. All statically linked, vendored, generated or dynamically downloaded components were found.
3. Names and versions map correctly to actual bytes.
4. The binary corresponds to reviewed source.
5. The listed dependency was the one used in the build.
6. The SBOM has not been replaced.
7. A VEX `not_affected` judgment remains valid after configuration changes.
8. The producer was authorized to release that artifact.

A sound pipeline therefore:

- Generates an SBOM as close as possible to the final artifact.
- Places it in an in-toto Statement whose `subject` is the artifact digest.
- DSSE-signs it with a trusted workload identity.
- Logs or bundles transparency evidence.
- Cross-checks it against SLSA `resolvedDependencies`, package-manager lockfiles, binary analysis and registry metadata.
- Verifies both artifact and SBOM at admission time.
- Treats VEX as signed, policy-evaluable evidence rather than as deletion of a scanner finding.

---

# 9. Binary transparency and the Go module example

## Append-only Merkle logs

Certificate-Transparency-style binary logs provide:

- **Inclusion:** artifact or signature \(x\) is committed under tree root \(R_n\).
- **Append-only consistency:** tree \(R_m\) is a prefix of later tree \(R_n\), \(m<n\).
- **Public auditability:** monitors can enumerate entries and flag unauthorized publication.
- **Equivocation detection:** gossip or witnesses compare checkpoints and detect two different roots for the same tree size.

RFC 9162 defines CT as an append-only Merkle tree of certificate entries. [web:58] For software, leaves may commit artifact hashes, signatures, certificates, provenance or release manifests.

Transparency does **not** by itself provide:

- confidentiality,
- correctness of logged content,
- authorization,
- freshness without trusted checkpoints/timestamps,
- split-view resistance without gossip/witnesses,
- availability.

A mature binary-transparency system should make the distributed artifact digest, signed release metadata, provenance digest and log leaf mutually referential so that a registry cannot quietly substitute one element.

## Go module proxy and checksum database

Go is a successful ecosystem-scale example of separating distribution from a global integrity record.

| Component | Function |
|---|---|
| **`GOPROXY` / `proxy.golang.org`** | Fetches and caches version lists, module metadata, `.mod` files and source ZIPs, improving availability and insulating builds from mutable or unavailable VCS hosts. |
| **`go.sum`** | Project-local expected hashes for downloaded module content and module metadata. |
| **`GOSUMDB=sum.golang.org`** | Global authenticated record of `go.sum` lines for public module versions. |
| **Trillian-backed transparent log** | Append-only Merkle structure supplying signed tree checkpoints, inclusion proofs and consistency proofs. |
| **`GOPRIVATE`/`GONOSUMDB`** | Excludes private module paths from public proxy/checksum queries where configured. |

When a module is not already pinned locally, the Go command does not simply trust the first proxy download. It obtains the expected checksum record from `sum.golang.org`, verifies that record’s inclusion under an authenticated signed tree head, and checks tree consistency before accepting new `go.sum` lines. The Go team describes sum.golang.org as a Trillian-backed transparent Merkle log and explicitly notes verification of both inclusion and consistency proofs. [web:46][web:52]

This stops a proxy or origin from silently serving different bytes for the same public module version to different victims once the version is recorded. It is “notary-like” rather than publisher-signature-based: the globally visible checksum log attests to a stable first-seen mapping from module version to content hash.

Its remaining limitations mirror other transparency systems:

- The first publicly recorded version can itself be malicious.
- A compromised maintainer may legitimately publish a new malicious version.
- Monitors/gossip are needed to detect split views.
- Private modules excluded from sumdb need an internal equivalent.
- It does not prove source review, reproducible compilation, or absence of malicious install/runtime behavior.

---

# Recommended end-to-end design

| Stage | Required control and cryptographic evidence |
|---|---|
| **Source** | Protected branches; mandatory review; signed release tag; immutable Git object ID; monitored maintainer changes |
| **Dependency resolution** | Lockfile plus registry/source identity and SHA-256 digest; no ambiguous public/private fallback; fetched objects retained or content-addressed |
| **Build** | Ephemeral SLSA L3-isolated builder; preferably hermetic; network deny-by-default; signing service inaccessible to build steps |
| **Provenance** | in-toto Statement with SLSA Provenance v1; exact artifact, source and dependency digests; complete external parameters |
| **Attestation signature** | DSSE PAE signature using a short-lived workload certificate or protected offline/HSM key |
| **Transparency** | Rekor or equivalent inclusion proof plus witnessed/gossiped signed checkpoint, saved in an offline bundle |
| **SBOM/VEX** | Signed SPDX 3 or CycloneDX attestation bound to final artifact digest; evidence-backed, expiring VEX |
| **Independent verification** | Multiple rebuilders reproduce exact distributable bytes; publish digest-bound results or VSA |
| **Release authorization** | Threshold-approved TUF targets metadata; snapshot consistency; short-lived timestamp freshness |
| **Deployment** | Admission policy checks digest, signer identity/issuer, builder ID, source repository/ref, provenance predicate, SBOM/VEX policy, transparency proof and TUF-authorized version |
| **Monitoring/recovery** | Watch OIDC identities and logs, gossip checkpoints, revoke compromised builders/CAs through TUF, retain historical policy and compromise-time data |

The decisive policy rule should be: **never authorize by signature validity alone**. Authorization must bind the exact artifact digest to an expected source, builder, workflow identity, provenance schema, release role, current version, and—where feasible—independent reproducibility result.

Citations:
[136] https://slsa.dev/spec/v1.0/levels
[137] https://slsa.dev/spec/v1.0/requirements
[138] https://slsa.dev/spec/v1.0/whats-new
[139] https://slsa.dev/spec/v1.0/faq
[140] https://slsa.dev/spec/v1.0-rc1/levels
[141] https://slsa.dev/blog/2023/02/slsa-v1-rc
[142] https://safeguard.sh/resources/blog/slsa-v1-1-build-track-adoption-2025
[143] https://www.practical-devsecops.com/slsa-framework-guide-software-supply-chain-security/
[144] https://slsa.dev/spec/v1.1/levels
[145] https://slsa.dev/spec/v1.0/about
[146] https://safeguard.sh/resources/blog/slsa-v1-1-framework-update-review
[147] https://helpmetest.com/blog/slsa-framework-supply-chain-integrity-levels/
[148] https://slsa.dev/current-activities
[149] https://slsa.dev/spec/v1.0-rc2/
[150] https://crashoverride.com/resources/knowledge-base/provenance/slsa-build-track-3-agent-artifacts
[121] https://github.com/in-toto/attestation/blob/main/spec/predicates/README.md
[122] https://slsa.dev/blog/2023/05/in-toto-and-slsa
[123] https://secure-pipelines.com/ci-cd-security/artifact-provenance-attestations-slsa-in-toto/
[124] https://github.com/in-toto/attestation
[125] https://static.sched.com/hosted_files/openssfcdna2025/93/OpenSSF%20Community%20Day%20NA%202025_%20in-toto%20policies%20%5BPUBLIC%5D.pdf
[126] https://safeguard.sh/resources/blog/in-toto-attestation-framework-walkthrough-2026
[127] https://techbytes.app/posts/in-toto-attestations-reference-verifiable-build-layouts/
[128] https://docs.xygeni.io/xygeni-products/build-security/salt-architecture
[129] https://www.ediverse.io/en/glossary/in-toto-attestations/
[130] https://bidda.com/intelligence/in-toto-attestation-framework-cncf
[131] https://sbomify.com/2024/08/14/what-is-in-toto/
[132] https://www.systemshardening.com/articles/cicd/container-image-attestations/
[133] https://safeguard.sh/resources/blog/in-toto-attestation-formats-review
[134] https://redhat-ads-tech.github.io/rhads-enablement-l3/modules/security-practices/ex3-provenance.html
[135] https://www.systemshardening.com/articles/cicd/slsa-attestation-admission-verification/
[31] https://docs.sigstore.dev/cosign/signing/overview/
[32] https://docs.sigstore.dev/cosign/system_config/custom_components/
[33] https://evertrust.io/guide/sigstore/
[34] https://oneuptime.com/blog/post/2026-08-26-distribute-private-fulcio-trust-root-tuf/view
[35] https://blog.sigstore.dev/sigstore-bring-your-own-stuf-with-tuf-40febfd2badd/
[36] https://docs.sigstore.dev/about/security/
[37] https://docs.sigstore.dev/about/faq/
[38] https://www.systemshardening.com/articles/kubernetes/sigstore-cosign-container-signing/
[39] https://amireslampanah.com/tutorials/cosign-sigstore-supply-chain.html
[40] https://blog.sigstore.dev/cosign-2-0-released/
[41] https://kloudvin.com/article/sigstore-keyless-signing-policy-controller-admission/
[42] https://docs.sigstore.dev/cosign/signing/gitsign/
[43] https://github.com/sigstore/gitsign/blob/main/README.md
[44] https://dev.to/kanywst/sigstore-deep-dive-unmasking-the-magic-behind-keyless-verification-lmh
[45] https://www.qcecuring.com/blog/sigstore-cosign-keyless-github-actions
[16] https://github.com/theupdateframework/specification/blob/master/tuf-spec.md
[17] https://theupdateframework.io/docs/faq/
[18] https://dev.to/havenmessenger/what-the-update-framework-assumes-will-go-wrong-2p07
[19] https://apis.io/apis/tuf/tuf-spec/
[20] https://deepwiki.com/theupdateframework/go-tuf/3-core-concepts
[21] https://apis.io/providers/tuf/
[22] https://deepwiki.com/theupdateframework/go-tuf/1-overview
[23] https://deepwiki.com/theupdateframework/go-tuf/3.2-metadata-roles
[24] https://standards.apievangelist.com/store/tuf/
[25] https://docs.rs/libdd-tuf/latest/tuf/
[26] https://deepwiki.com/theupdateframework/rust-tuf/6.3-attack-mitigations
[27] https://kameniksolutions.com/garden/the-update-framework-tuf/
[28] https://theupdateframework.io/docs/metadata/
[29] https://medium.com/@mulgundmath/secure-software-updates-via-tuf-part-2-412c6a2b10ab
[30] https://theupdateframework.github.io/python-tuf/2022/06/15/testing-ngclient.html
[1] https://reproducible-builds.org/reports/2025-03/
[2] https://reproducible-builds.org/_lfs/presentations/2025-10-21-Reproducible-Builds-brief-summary-of-12-years-and-a-glimpse-into-the-future/
[3] https://reproducible-builds.org/reports/2025-04/
[4] https://reproducible-builds.org/reports/2025-01/
[5] https://lists.reproducible-builds.org/pipermail/rb-general/2024-June/003436.html
[6] https://reproducible-builds.org/success-stories/
[7] https://reproducible-builds.org/docs/commandments/
[8] https://discussion.fedoraproject.org/t/f43-change-proposal-package-builds-are-expected-to-be-reproducible-system-wide/147320
[9] https://luj.fr/assets/20260522T000000==public--decade-reproducibility-paper.pdf
[10] https://nesbitt.io/2026/02/24/reproducible-builds-in-language-package-managers.html
[11] https://reproduce.debian.net/
[12] https://reproducible-builds.org/reports/2025-09/
[13] https://reproducible-builds.org/reports/2025-06/
[14] https://senior-stack.uz/Roadmap/Programming/quality-engineering/build-systems/09-reproducible-builds/middle/
[15] https://reproducible-builds.org/reports/2024-03/
[76] https://www.crowdstrike.com/en-us/blog/sunspot-malware-technical-analysis/
[77] https://mattmccarthy.dev/blog/xz-backdoor-supply-chain-lessons
[78] https://www.eviant.com.au/resources/solarwinds-sunburst-backdoor-technical-analysis-supply-chain/
[79] https://kpmg.com/us/en/articles/2023/solarwinds-explainer.html
[80] https://access.redhat.com/security/cve/cve-2024-3094
[81] https://www.rapid7.com/blog/post/2021/01/12/update-on-solarwinds-supply-chain-attack-sunspot-and-new-malware-family-associations/
[82] https://krebsonsecurity.com/2021/01/solarwinds-what-hit-us-could-hit-others/
[83] https://exzilcalanza.info/xz-utils-backdoor-cve-2024-3094-technical-anatomy-obfuscation/
[84] https://threatpedia.wiki/campaigns/solarwinds-supply-chain-campaign/
[85] https://owasp.org/www-chapter-croatia/assets/files/2021-02-SolarWindsOWASP.pdf
[86] https://www.itnews.com.au/news/researchers-detail-solarwinds-code-injection-attack-559662
[87] https://www.solarwinds.com/blog/an-investigative-update-of-the-cyberattack
[88] https://dspace.cvut.cz/bitstream/handle/10467/124000/F8-BP-2025-Matejckova-Anna-matejckova.pdf?sequence=-1&isAllowed=y
[89] https://www.crowdstrike.com/ja-jp/press-releases/sunspot-malware-technical-analysis/
[90] https://www.rescana.com/post/in-depth-analysis-supply-chain-poisoning-of-popular-npm-packages-exploiting-event-stream-ua-parser
[61] https://securitylabs.datadoghq.com/articles/shai-hulud-2.0-npm-worm/
[62] https://www.cisa.gov/news-events/alerts/2025/09/23/widespread-supply-chain-compromise-impacting-npm-ecosystem
[63] https://krebsonsecurity.com/2025/09/self-replicating-worm-hits-180-software-packages/
[64] https://unit42.paloaltonetworks.com/npm-supply-chain-attack/
[65] https://labs.cloudsecurityalliance.org/research/csa-research-note-shai-hulud-npm-worm-ai-developer-supply-ch/
[66] https://checkmarx.com/zero-post/npm-hit-by-shai-hulud-the-self-replicating-supply-chain-attack/
[67] https://labs.cloudsecurityalliance.org/wp-content/uploads/2026/04/CSA_research_note_shai-hulud-npm-worm-ai-developer-supply-chain_20260425-csa-styled-2.pdf
[68] https://thehackernews.com/2025/09/40-npm-packages-compromised-in-supply.html
[69] https://www.sysdig.com/blog/shai-hulud-the-novel-self-replicating-worm-infecting-hundreds-of-npm-packages
[70] https://labs.cloudsecurityalliance.org/research/csa-research-note-shai-hulud-ai-supply-chain-20260517-csa-st/
[71] https://panther.com/blog/shai-hulud-npm-supply-chain-attack
[72] https://xygeni.io/blog/shai-hulud-the-npm-packages-worm-explained/
[73] https://www.helpnetsecurity.com/2025/09/16/self-replicating-worm-hits-180-npm-packages-in-largely-automated-supply-chain-attack/
[74] https://www.invicti.com/blog/web-security/shai-hulud-2-worm-supply-chain-attack-on-npm-dependencies
[75] https://www.wiz.io/blog/shai-hulud-npm-supply-chain-attack
[91] https://git-scm.com/docs/hash-function-transition
[92] https://rodhat.com/motd/2026-08-10-git-sha256-finally-moving/
[93] https://codeoxi.com/blog/whats-new-in-git-2026
[94] https://www.phoronix.com/news/Git-2.45-rc0-Released
[95] https://www.devclass.com/development/2023/08/22/git-242-released-sha-256-repositories-no-longer-an-experimental-curiosity/1627796
[96] https://www.ossflow.fr/git-3-0-and-beyond-whats-coming/
[97] https://lwn.net/Articles/811068/
[98] https://stackoverflow.com/questions/60087759/git-is-moving-to-new-hashing-algorithm-sha-256-but-why-git-community-settled-on
[99] https://lwn.net/Articles/898522/
[100] https://news.ycombinator.com/item?id=31851755
[101] https://www.theregister.com/2020/02/05/git_sha_256_work/
[102] https://lwn.net/Articles/1057382/
[103] https://www.linkedin.com/pulse/gits-sha-1-problem-inside-move-sha-256-himanshu-bhaadoriya-hxstc
[104] https://lwn.net/Articles/1042172/
[105] https://www.phoronix.com/news/Git-2.52-rc0-Released
[106] https://cyclonedx.org/specification/overview/
[107] https://cyclonedx.org/
[108] https://spdx.dev/wp-content/uploads/sites/31/2024/12/SPDX-3.0.1-1.pdf
[109] https://spdx.dev/use/specifications/
[110] https://cyclonedx.org/capabilities/vex/
[111] https://github.com/CycloneDX/specification/blob/master/schema/spdx.xsd
[112] https://techbytes.app/posts/sbom-standards-remediation-cheat-sheet-devsecops-2026/
[113] https://github.com/CycloneDX/specification
[114] https://cyclonedx.captnemo.in/capabilities/vex/
[115] https://spdx3.licquia.org/pdf/combined.pdf
[116] https://www.diva-portal.org/smash/get/diva2:2073107/FULLTEXT01.pdf
[117] https://haksungjang.github.io/en/docs/sbom_guide/2-standards/
[118] https://cyclonedx.org/news/cyclonedx-v1.4-released/
[119] https://tessl.io/registry/testland/sbom-formats/files/references/spdx.md
[120] https://github.com/cyclonedx
[46] https://go.dev/blog/module-mirror-launch
[47] https://go.googlesource.com/proposal/+/master/design/25530-sumdb.md
[48] https://arriqaaq.medium.com/dont-trust-your-logs-implementing-a-merkle-tree-for-an-immutable-verifiable-log-in-go-c242b558ae00
[49] https://news.ycombinator.com/item?id=46539153
[50] https://safeguard.sh/resources/blog/go-module-checksum-database-security
[51] https://golang-blog.blogspot.com/2019/11/module-mirror-checksum-database.html
[52] https://go.dev/ref/mod
[53] https://byteincrements.com/2025/07/29/demystifying-go-proxy-and-checksum-database/
[54] https://golang-blog.blogspot.com/2021/03/authenticating-modules-checksum-database.html?m=0
[55] https://www.ietf.org/archive/id/draft-zhang-trans-ct-binary-codes-04.txt
[56] https://safeguard.sh/resources/blog/go-module-proxy-security-how-goproxy-and-sumgolangorg-protect-and-can-fail-integrity
[57] https://freenode.net/article/go-sumdb-fixes-reopen-who-anchors-module-trust
[58] https://www.rfc-editor.org/rfc/rfc9162.pdf
[59] https://tessl.io/registry/tessl/golang-golang-org-x--mod/0.30.0/files/docs/tlog.md
[60] https://www.net.in.tum.de/fileadmin/TUM/NET/NET-2020-11-1/NET-2020-11-1_09.pdf
[151] https://docs.sigstore.dev/about/threat-model/
[152] https://www.systemshardening.com/articles/cicd/sigstore-keyless-signing/
[153] https://blog.sigstore.dev/privacy-in-sigstore-57cac15af0d0/
[154] https://blog.sigstore.dev/cosign-2-0-released/
[155] https://docs.sigstore.dev/cosign/signing/overview/
[156] https://arxiv.org/pdf/2503.00271
[157] https://app.opencve.io/cve/?page=2&vendor=sigstore
[158] https://docs.sigstore.dev/logging/overview/
[159] https://ostif.org/wp-content/uploads/2022/07/OSTIF-2022-Q1-Sigstore-Report.pdf
[160] https://git.stella-ops.org/stella-ops.org/git.stella-ops.org/src/commit/2c27c7673fa0584d962586320f5b67234163f894/docs/operations/checkpoint-divergence-runbook.md
[161] https://arxiv.org/html/2605.03309v1
[162] https://dl.acm.org/doi/pdf/10.1145/3548606.3560596
[163] https://docs.nvidia.com/aicr/contributor-guide/rekor-v-2-signing/
[164] https://news.ycombinator.com/item?id=33332636
[165] https://github.com/sigstore/docs/blob/main/content/en/about/faq.md
[211] https://reproducible-builds.org/blog/posts/135/
[212] https://lists.reproducible-builds.org/pipermail/rb-general/2024-March/003291.html
[213] https://outpost.fosspost.org/d/18-research-paper-tests-how-many-arch-linux-packages-are-reproducible
[214] https://arxiv.org/html/2505.21642v1
[215] https://lists.reproducible-builds.org/pipermail/rb-general/2024-March/003301.html
[216] https://wiki.archlinux.org/title/Reproducible_builds
[217] https://www.reddit.com/r/archlinux/comments/1jen23x/how_can_package_builds_be_trusted/
[218] https://reproducible.archlinux.org/
[219] https://reproducible-builds.org/events/vienna2025/agenda/project-updates/
[220] https://mcis.cs.queensu.ca/publications/2024/emse_rahul.pdf
[221] https://lists.reproducible-builds.org/pipermail/rb-general/2025-January/003625.html
[222] https://www.reddit.com/r/archlinux/comments/nkzerd/can_you_install_arch_with_reproductible_builds/
[223] https://bbs.archlinux.org/viewtopic.php?id=270319
[224] https://reproducible-builds.org/reports/2026-05/
[225] https://wiki.archlinux.org/title/Reproducible_builds/Status
[196] https://luj.fr/assets/20260522T000000==public--decade-reproducibility-paper.pdf
[197] https://upsilon.cc/~zack/research/publications/icse-nier-2024-nix.pdf
[198] https://arxiv.org/html/2501.15919v1
[199] https://arxiv.org/html/2601.20662v1
[200] https://luj.fr/blog/is-nixos-truly-reproducible.html
[201] https://discourse.nixos.org/t/reproducibility-of-build-environments-through-space-and-time/40043
[202] https://news.ycombinator.com/item?id=42989666
[203] https://researchportal.ip-paris.fr/fr/publications/does-functional-package-management-enable-reproducible-builds-at-/
[204] https://safeguard.sh/resources/blog/nix-reproducible-builds-supply-chain-case
[205] https://luj.fr/garden/20250127T000000==public--is-nixos-truly-reproducible__blog.org
[206] https://reproducible-builds.org/reports/2024-02/
[207] https://reproducible-builds.org/reports/2025-01/
[208] https://www.themoonlight.io/en/review/does-functional-package-management-enable-reproducible-builds-at-scale-yes
[209] https://luj.fr/assets/talks/ICSE_NIER.pdf
[210] https://nixos.org/research/
[301] https://fedoraproject.org/wiki/Changes/Package_builds_are_expected_to_be_reproducible
[302] https://docs.fedoraproject.org/en-US/reproducible-builds/
[303] https://www.opensourcefeed.org/fedora-linux-44-beta-released/
[304] https://cfp.fedoraproject.org/flock-to-fedora-2025/talk/RTE9E9/
[305] https://windowsforum.com/threads/fedora-44-beta-gnome-50-and-plasma-6-6-with-wayland-first-desktop.404751/
[306] https://lists.fedoraproject.org/archives/list/devel@lists.fedoraproject.org/thread/NRRAH67FZXVKGERYTBSVL6QIQ57BXXBG/
[307] https://www.phoronix.com/news/Fedora-44-Reproducible-Builds
[308] https://blog.desdelinux.net/en/Fedora-43-is-preparing-to-build-fully-reproducible-packages./
[309] https://ostechnix.com/upgrade-to-fedora-44-from-fedora-43/
[310] https://reproducible-builds.org/reports/2025-03/
[311] https://www.reddit.com/r/linux/comments/1jwy46e/fedora_change_aims_for_99_package_reproducibility/
[312] https://www.webpronews.com/fedora-delays-reproducible-rpm-builds-mandate-to-fedora-44-for-security/
[313] https://www.osnews.com/story/142113/fedora-change-aims-for-99-package-reproducibility/
[314] https://linux.slashdot.org/story/25/04/11/2143211/fedora-targets-99-package-reproducibility-by-october
[315] https://cloudsmith.com/blog/reproducible-builds-fedora-43-and-what-it-means-for-the-software-supply-chain
[241] https://reproducible-builds.org/docs/source-date-epoch/
[242] https://reproducible-builds.org/specs/source-date-epoch/
[243] https://reproducible-builds.org/docs/timestamps/
[244] https://rb.mapreri.org/site/docs/source-date-epoch/
[245] https://docs.docker.com/build/ci/github-actions/reproducible-builds/
[246] https://lists.buildroot.org/pipermail/buildroot/2016-November/542295.html
[247] https://github.com/mkdocs/mkdocs/issues/938
[248] https://archive.fosdem.org/2023/schedule/event/container_reproducible_dockerfile/attachments/slides/5574/export/events/attachments/container_reproducible_dockerfile/slides/5574/FOSDEM2023_Bit_for_bit_reproducible_builds_with_Dockerfile.pdf
[249] https://docs.u-boot.org/en/stable/build/reproducible.html
[250] https://docs.u-boot.org/en/latest/build/reproducible.html
[251] https://reviews.llvm.org/D20791
[252] https://arxiv.org/html/2104.06020v1
[253] https://www.arxiv.org/pdf/2505.21642
[254] https://wiki.archlinux.org/title/Rebuilderd
[255] https://github.com/openjdk/jdk11u-dev/commit/ffea91377b86ed221c9d4bdd6db351f8654ad6a0
[316] https://blog.npmjs.org/post/180565383195/details-about-the-event-stream-incident
[317] https://www.cisa.gov/news-events/alerts/2021/10/22/malware-discovered-popular-npm-package-ua-parser-js
[318] https://safeguard.sh/resources/blog/ua-parser-js-npm-hijack-october-2021
[319] https://snyk.io/blog/a-post-mortem-of-the-malicious-event-stream-backdoor/
[320] https://medium.com/@hkparker/analysis-of-a-supply-chain-attack-2bd8fa8286ac
[321] https://safeguard.sh/resources/blog/event-stream-flatmap-stream-npm-backdoor-incident
[322] https://safeguard.sh/resources/blog/lessons-from-ua-parser-js-compromise
[323] https://thehackernews.com/2021/10/popular-npm-package-hijacked-to-publish.html
[324] https://safeguard.sh/resources/blog/ua-parser-js-npm-hijack-incident
[325] https://www.crowdstrike.com/en-us/blog/crowdstrike-customers-protected-from-compromised-npm-package-in-supply-chain-attack/
[326] https://safeguard.sh/resources/blog/lessons-from-event-stream-npm-attack
[327] https://www.rapid7.com/blog/post/2021/10/25/npm-library-ua-parser-js-hijacked-what-you-need-to-know/
[328] https://dl.acm.org/doi/pdf/10.1145/3517208.3523753
[329] https://jaydenseric.com/blog/event-stream-compromise
[330] https://www.sonatype.com/blog/nexus-intelligence-insights-sonatype-2018-0413
[286] https://about.codecov.io/apr-2021-post-mortem/
[287] https://www.cisa.gov/news-events/alerts/2021/04/30/codecov-releases-new-detections-supply-chain-compromise
[288] https://www.cisa.gov/news-events/alerts/2023/03/30/supply-chain-attack-against-3cxdesktopapp
[289] https://www.cisa.gov/news-events/alerts/2023/04/20/cisa-releases-malware-analysis-report-iconicstealer
[290] https://techcrunch.com/2023/04/20/3cx-supply-chain-xtrader-mandiant/
[291] https://cloud.google.com/blog/topics/threat-intelligence/3cx-software-supply-chain-compromise
[292] https://blog.gitguardian.com/codecov-supply-chain-breach/
[293] https://www.washingtonpost.com/politics/2023/04/20/3cx-cyberattack-was-result-two-supply-chain-hacks-mandiant-says/
[294] https://www.rapid7.com/blog/post/2021/04/16/codecov-discloses-supply-chain-compromise/
[295] https://safeguard.sh/resources/blog/codecov-bash-uploader-supply-chain-attack-anatomy
[296] https://safeguard.sh/resources/blog/the-codecov-supply-chain-attack-explained
[297] https://www.cnn.com/2023/04/20/politics/north-korea-hacking-supply-chain-3cx-mandiant
[298] https://threats.wiz.io/all-incidents/codecov-incident
[299] https://www.techtarget.com/cybersecurity/news/365535424/Mandiant-3CX-breach-caused-by-second-supply-chain-attack
[300] https://safeguard.sh/resources/blog/codecov-bash-uploader-supply-chain-breach
[256] https://xakep.ru/2021/02/11/dependency-confusion/
[257] https://www.malwarebytes.com/blog/news/2021/02/researchers-audacious-hack-demonstrates-new-type-of-supply-chain-attack
[258] https://simonwillison.net/2021/Feb/10/
[259] https://safeguard.sh/resources/blog/dependency-confusion-attack
[260] https://arxiv.org/pdf/2605.03309
[261] https://safeguard.sh/resources/blog/dependency-confusion-attacks-five-years-later-are-enterprises-actually-protected
[262] https://safeguard.sh/resources/blog/dependency-confusion-and-npm-supply-chain-hardening
[263] https://www.securityscientist.net/blog/12-questions-and-answers-about-dependency-confusion-supply-chain/
[264] https://safeguard.sh/resources/blog/anatomy-of-an-npm-dependency-confusion-attack
[265] https://www.sonatype.com/blog/why-are-dependency-confusion-attacks-not-going-away
[266] https://discuss.python.org/t/typosquatting-dependency-confusion-supply-chain-attack-call-it-as-you-wish/52615
[267] https://docs.npmjs.com/threats-and-mitigations/
[268] https://secportal.io/vulnerabilities/dependency-confusion
[269] https://dhiyaneshgeek.github.io/web/security/2021/09/04/dependency-confusion/
[270] https://www.usenix.org/system/files/usenixsecurity23-neupane.pdf
[181] https://www.akamai.com/blog/security-research/critical-linux-backdoor-xz-utils-discovered-what-to-know
[182] https://cert.europa.eu/publications/security-advisories/2024-032/pdf
[183] https://securelist.com/xz-backdoor-story-part-1/112354/
[184] https://www.openeuler.org/en/blog/cve-3094.html
[185] https://www.sentinelone.com/blog/xz-utils-backdoor-threat-actor-planned-to-inject-further-vulnerabilities/
[186] https://blog.csdn.net/2301_81475873/article/details/139745322
[187] https://lwn.net/Articles/967192/
[188] https://gist.github.com/thesamesam/223949d5a074ebc3dce9ee78baad9e27
[189] https://www.stepsecurity.io/blog/analysis-of-backdoored-xz-utils-build-process-with-harden-runner
[190] https://www.openwall.com/lists/oss-security/2024/03/29/4
[191] https://arxiv.org/html/2404.08987v1
[192] https://seclists.org/oss-sec/2024/q1/340
[193] https://cyberwarfare.live/unmasking-the-xz-utils-vulnerability/
[194] https://media.defcon.org/DEF%20CON%2032/DEF%20CON%2032%20presentations/DEF%20CON%2032%20-%20Thomas%20Roccia%20-%20The%20XZ%20Backdoor%20Story%20The%20Undercover%20Operation%20That%20Set%20the%20Internet%20on%20Fire.pdf
[195] https://www.softwareseni.com/the-xz-utils-backdoor-cve-2024-3094-and-the-multi-year-social-engineering-campaign-behind-it/
[226] https://docs.github.com/en/authentication/managing-commit-signature-verification/signing-tags
[227] https://docs.github.com/en/authentication/managing-commit-signature-verification/about-commit-signature-verification
[228] https://havenmessenger.com/blog/posts/git-commit-signing-explained/
[229] https://docs.github.com/en/enterprise-server@3.10/authentication/managing-commit-signature-verification/signing-tags
[230] https://docs.github.com/en/enterprise-server@3.20/authentication/managing-commit-signature-verification/signing-tags
[231] https://docs.github.com/en/enterprise-server@3.19/authentication/managing-commit-signature-verification/signing-tags
[232] https://docs.github.com/en/enterprise-server@3.14/authentication/managing-commit-signature-verification/signing-tags
[233] https://docs.github.com/en/enterprise-server@3.22/authentication/managing-commit-signature-verification/signing-tags
[234] https://docs.github.com/de/enterprise-server@3.15/authentication/managing-commit-signature-verification/about-commit-signature-verification
[235] https://www.git-automation.com/commit-signing-supply-chain-security/
[236] https://git-scm.com/book/ms/v2/Git-Tools-Signing-Your-Work
[237] https://docs.github.com/en/enterprise-server@3.8/authentication/managing-commit-signature-verification/about-commit-signature-verification
[238] https://docs.gitlab.com/user/project/repository/signed_commits/x509/
[239] https://docs.github.com/en/authentication/managing-commit-signature-verification
[240] https://git-scm.com/docs/git-tag
[166] https://slsa.dev/provenance/v1
[167] https://slsa.dev/spec/v0.1/verification_summary
[168] https://slsa.dev/spec/v1.0/provenance
[169] https://slsa.dev/spec/v0.2/verification_summary
[170] https://slsa.dev/spec/v1.0-rc1/provenance
[171] https://slsa.dev/spec/v1.1/provenance
[172] https://slsa.dev/verification_summary
[173] https://slsa.dev/spec/v1.1/
[174] https://slsa.dev/spec/v1.2/verification_summary
[175] https://slsa.dev/spec/v1.2-rc2/verification_summary
[176] https://slsa.dev/spec/draft/build-provenance
[177] https://slsa.dev/spec/v1.0/whats-new
[178] https://slsa.dev/spec/v1.0/distributing-provenance
[179] https://supraj.dev/guides/slsa-v12-production-deep-dive/
[180] https://slsa.dev/spec/v1.2/build-provenance
[271] https://github.com/secure-systems-lab/dsse/blob/master/protocol.md
[272] https://github.com/secure-systems-lab/dsse/blob/v1.0.0/protocol.md
[273] https://github.com/secure-systems-lab/dsse/blob/master/background.md
[274] https://pkg.go.dev/github.com/secure-systems-lab/go-securesystemslib/dsse
[275] https://docs.rs/leyline-envelope/latest/leyline_envelope/
[276] https://github.com/in-toto/attestation/blob/main/spec/v1/envelope.md
[277] https://github.com/secure-systems-lab/dsse/blob/master/envelope.proto
[278] https://pkg.go.dev/github.com/shizhMSFT/go-dsse
[279] https://deepwiki.com/jdx/sigstore-verification/6.2-cryptographic-signature-verification
[280] https://fossies.org/linux/moby-docker/vendor/github.com/secure-systems-lab/go-securesystemslib/dsse/envelope.go
[281] https://nono.sh/docs/cli/internals/signing
[282] https://github.com/secure-systems-lab/dsse
[283] https://k2gl.com/packages/dsse.html
[284] https://docs.rs/wsc-dsse/latest/src/wsc_dsse/lib.rs.html
[285] https://docs.rs/atlas-cli/latest/src/atlas_cli/in_toto/dsse.rs.html
[331] https://uptane.org/docs/latest/standard/uptane-standard
[332] https://uptane.org/papers/uptane-standard.2.0.0.pdf
[333] https://uptane.org/papers/uptane-standard.1.1.0.pdf
[334] https://uptane.org/docs/2.1.0/deployment/best-practices
[335] https://uptane.org/docs/1.0.0/standard/uptane-standard
[336] https://www.scientificbulletin.upb.ro/static/pdfs/fullb27_956255.pdf
[337] https://uptane.org/enhancements/pouf/pouf1
[338] https://github.com/uptane/papers/blob/master/uptane-whitepaper.md
[339] https://uptane.org/papers/uptane-deployment-best-practices-1.1.0.html
[340] https://uptane.org/docs/2.1.0/standard/uptane-standard
[341] https://uptane.org/papers/uptane-standard.2.1.0.pdf
[342] https://uptane.org/assets/files/22uptane_yokohama_december_7-55625da482a4a3b10dc734e165b87c64.pdf
[343] https://github.com/awwad/uptane
[344] https://repositorio-aberto.up.pt/bitstream/10216/147646/2/605924.pdf
[345] https://uptane.org/assets/files/IIW21_session1-a19f7b70be73608960bb7f448ba17d34.pdf
[376] https://slsa.dev/spec/v1.0/whats-new
[377] https://slsa.dev/blog/2025/04/slsa-v1.1
[378] https://slsa.dev/spec/v1.1/whats-new
[379] https://slsa.dev/spec/v1.1-rc1/whats-new
[380] https://slsa.dev/blog/2025/04/slsa-v1.1-rc2
[381] https://slsa.dev/blog
[382] https://slsa.dev/spec/v1.1/
[383] https://slsa.dev/spec/draft/whats-new
[384] https://safeguard.sh/resources/blog/slsa-v1-1-framework-update-review
[385] https://slsa.dev/blog/2023/02/slsa-v1-rc
[386] https://safeguard.sh/resources/blog/slsa-v1-1-build-track-adoption-2025
[387] https://slsa.dev/blog/2023/04/slsa-v1-rc2
[388] https://slsa.dev/spec/v1.1/about
[389] https://slsa.dev/spec/v1.2/whats-new
[390] https://slsa.dev/blog/2023/04/slsa-v1-final
[391] https://github.com/in-toto/docs/blob/master/in-toto-spec.md
[392] https://in-toto.io/docs/getting-started/
[393] https://pkg.go.dev/github.com/in-toto/in-toto-golang/in_toto
[394] https://in-toto.readthedocs.io/en/latest/model.html
[395] https://www.usenix.org/system/files/sec19-torres-arias.pdf
[396] https://in-toto.readthedocs.io/en/latest/command-line-tools/in-toto-verify.html
[397] https://perun.au/insights/in-toto-production
[398] https://slsa.dev/blog/2023/05/in-toto-and-slsa
[399] https://docs.rs/in-toto/latest/in_toto/verifylib/fn.in_toto_verify.html
[400] https://in-toto.readthedocs.io/en/stable/_sources/layout-creation-example.md.txt
[401] https://shibumi.dev/posts/introduction-to-in-toto/
[402] https://techbytes.app/posts/in-toto-attestations-reference-verifiable-build-layouts/
[403] https://in-toto.github.io/specs.html
[404] https://github.com/in-toto/specification
[405] https://in-toto.io/docs/examples/debian/
[361] https://www.mn.uio.no/ifi/english/research/groups/psy/completedmasters/2018/skrimstad/presentation-skrimstad.pdf
[362] https://kth.diva-portal.org/smash/get/diva2:1998446/FULLTEXT01.pdf
[363] https://saschafahl.de/static/paper/reprobuilds2023.pdf
[364] http://enck.org/pubs/benedetti-icse25.pdf
[365] https://www.mn.uio.no/ifi/english/research/groups/psy/completedmasters/2018/skrimstad/
[366] https://arxiv.org/html/2608.27125
[367] https://arxiv.org/abs/2505.21642
[368] https://reproducible-builds.org/docs/why/
[369] https://reproducible-builds.org/
[370] https://arxiv.org/html/2602.19383v1
[371] https://www.systemshardening.com/articles/linux/reproducible-builds-security/
[372] https://dwheeler.com/trusting-trust/
[373] https://www.cleanstart.com/knowledge-hub/what-are-reproducible-builds
[374] https://safeguard.sh/resources/blog/what-is-reproducible-builds
[375] https://arxiv.org/abs/2104.06020
[406] https://nesbitt.io/2026/02/24/reproducible-builds-in-language-package-managers.html
[407] https://reproduce.debian.net/
[408] https://micronews.debian.org/2025/1746302888.html
[409] https://reproduce.debian.net/armel/stats/trixie/
[410] https://patchwindow.serverdigital.net/brief/debian-14-reproducible-builds-required
[411] https://reproducible-builds.org/reports/2026-02/
[412] https://reproducible-builds.org/reports/2026-05/
[413] https://thecodersblog.com/debian-reproducible-packages-2026/
[414] https://needhelp.icu/blogs/debian-reproducible-builds-mandate
[415] https://lists.debian.org/debian-devel-announce/2026/05/msg00001.html
[416] https://safeguard.sh/resources/blog/reproducible-builds-debian-long-view
[417] https://lwn.net/Articles/657479/
[418] https://reproducible-builds.org/reports/2025-06/
[419] https://reproduce.debian.net/all/api/v1/builds/179279/log
[420] https://www.linkedin.com/posts/debian_progress-on-securing-our-distribution-against-activity-7324577295384403968-ISTZ
[421] https://oneuptime.com/blog/post/2026-08-11-rekor-cosign-verification-transparency-log-outage/view
[422] https://www.qcecuring.com/blog/sigstore-cosign-keyless-github-actions
[423] https://docs.sigstore.dev/cosign/signing/overview/
[424] https://factory.ai/open-source-wikis/cosign?page=cli/verification-commands.md
[425] https://factory.com/open-source-wikis/cosign
[426] https://github.com/sigstore/cosign/security/advisories/GHSA-8gw7-4j42-w388
[427] https://github.com/sigstore/cosign/issues/181
[428] https://kyverno.io/docs/policy-types/cluster-policy/verify-images/sigstore/
[429] https://kloudvin.com/article/sigstore-keyless-signing-policy-controller-admission/
[430] https://devopsaitoolkit.com/blog/cosign-bundle-air-gapped-verification/
[431] https://github.com/sigstore/rekor/issues/257
[432] https://docs.github.com/en/enterprise-cloud@latest/actions/security-for-github-actions/using-artifact-attestations/verifying-attestations-offline
[433] https://www.augmentedmind.de/2025/03/02/docker-image-signing-with-cosign/
[434] https://docs.sigstore.dev/cosign/verifying/verify/
[435] https://pkijs.com/sigstore
[346] https://su3.io/posts/witnessing-sigstore-from-ethereum
[347] https://safeguard.sh/resources/blog/rekor-transparency-log
[348] https://safeguard.sh/resources/blog/sigstore-rekor-transparency-log-operations
[349] https://deepwiki.com/sigstore/sigstore-rs/3.3-signature-operations
[350] https://github.com/sigstore/architecture-docs/blob/main/rekor-v2-spec.md
[351] https://safeguard.sh/resources/blog/sigstore-rekor-transparency-log-deep-dive-2026
[352] https://docs.sigstore.dev/about/security/
[353] https://deepwiki.com/jdx/sigstore-verification/10.1-rekor-transparency-log-verification
[354] https://deepwiki.com/sigstore/sigstore-rs/2.3-rekor-integration
[355] https://raw.githubusercontent.com/sigstore/rekor/main/openapi.yaml
[356] https://edu.chainguard.dev/open-source/sigstore/rekor/an-introduction-to-rekor/
[357] https://deepwiki.com/sigstore/docs/6-rekor-transparency-log
[358] https://blog.stephane-robert.info/docs/securiser/supply-chain/rekor/
[359] https://google.github.io/trillian/docs/TransparentLogging.html
[360] https://www.rfc-editor.org/rfc/rfc6962.html
