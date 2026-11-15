# Transparency timestamp verification

## Decision

Liquid implements the RFC 3161 and RFC 4998 verification path itself. The implementation is
verification only: it parses strict DER, validates CMS `SignedData` and X.509 paths, and verifies
the RSA and NIST-curve ECDSA signatures used by timestamp authorities. It does not generate keys,
certificates, or signatures. It does not add a general-purpose cryptography dependency.

This is bridge decision 32, selected by Steve on 2026-09-05. Keeping the verifier in libliquid
matches the existing decisions to own the chunk index, transport, and protobuf codec while keeping
the public library independent of lake and Zetta.

## Lake evidence contract

Lake returns a DER RFC 4998 `EvidenceRecord` version 1 with one SHA-256 digest algorithm and one
archive-timestamp chain. The first RFC 3161 token carries the SHA-256 digest of the decoded lake
checkpoint signature value. Each renewal token carries the SHA-256 digest of the complete DER
encoding of the immediately preceding `ArchiveTimeStamp`. A checkpoint has no reduced hash tree.

The verifier will reject BER encodings, indefinite lengths, non-minimal DER lengths and integers,
unknown critical certificate extensions, unsupported digest or signature algorithms, an invalid
timestamp-signing purpose, a certificate path that does not end at a configured trust anchor, a
certificate outside its validity period at the token's generation time, a bad CMS signature, a
changed token, and a broken renewal link. Trust anchors default to an empty set; clients must pin
the timestamp authorities they accept.

Historical verification evaluates every timestamp and its signing path at that token's recorded
generation time. A certificate that has expired by the current wall clock can therefore preserve
a previously valid token, while every later archive timestamp must still cover the exact prior DER
value.

## Implementation checkpoint

`liquid/asn1.hpp` exposes the strict DER reader. It refuses indefinite and non-minimal lengths,
non-minimal high tag numbers, truncated values, negative or padded unsigned integers, malformed
object identifiers, and trailing data after a requested single element. Each returned element
retains both its content span and its complete encoded span. The remaining public headers,
command-line operations, recorded fixtures, and verification implementation are delivered by
liquid issue #210.

The private signature engine accepts RSA keys from 2,048 through 4,096 bits and ECDSA public keys
on P-256 or P-384. It verifies PKCS#1 v1.5, RSA-PSS, and DER ECDSA signatures with SHA-256 or
SHA-384. SHA-384 and each signature scheme are verification-only; there is no key-generation or
signing entry point.

`liquid/x509.hpp` parses DER certificates, accepts one or more DER or PEM trust anchors, and builds
a path at an explicitly supplied historical time. The timestamp leaf must permit digital
signatures and carry a critical extended-key-usage extension containing only
`id-kp-timeStamping`. Issuing certificates must be certificate authorities with `keyCertSign`.
Unknown critical extensions, invalid certificate signatures, validity failures, loops, and paths
that do not end in the configured anchor set are refused. An empty anchor set returns a named
authorization failure; system roots are never an implicit default.

`liquid/cms.hpp` verifies an embedded-content CMS `SignedData` value with exactly one signer. It
requires canonical DER ordering for digest, signer, and signed-attribute sets; binds the signed
`contentType` and `messageDigest` attributes to `id-ct-TSTInfo`; identifies the signer by issuer
and serial number; verifies its signature; and validates its certificate path at the caller's
historical time.

`liquid/transparency.hpp` exposes the timestamp and evidence operations. A timestamp token hashes
the supplied decoded checkpoint signature value with SHA-256 and compares it to the signed
`messageImprint`; requires a signed nonce, policy, serial number, and generation time; then verifies
the CMS signature and TSA path at that generation time. Evidence verification accepts the complete
DER record plus the decoded checkpoint signature value. It requires lake's version-1, one-algorithm,
one-chain shape and verifies the first token over that signature value. Every later token is
verified over the complete preserved DER of its immediate `ArchiveTimeStamp` predecessor.

The same two operations date TAAP signatures for compromised keys (liquid #391):
`liquid::trusted_signature_time` in `liquid/identity.hpp` passes a signature's bytes as the covered
value and returns the token's (or the initial archive timestamp's) generation time, and
`verify_with_trusted_time` accepts a signature by a declared-compromised key only when that time is
strictly before the declaration ([identity.md](identity.md#rotation)).

## Command line

`liquid log timestamps <log-url>` fetches the current checkpoint and its lake-produced timestamp
evidence for inspection. `liquid log verify --timestamps` takes its endpoint from `LIQUID_LOG_URL`,
loads pinned PEM anchors from `LIQUID_TSA_TRUST_CERT`, fetches both records, decodes their base64url
values strictly, and calls `evidence_record::verify`. `LIQUID_TOKEN` is forwarded as an optional
bearer credential. No TSA is trusted by default, and system certificate roots are not consulted.

The timestamp command verifies the RFC 3161 and RFC 4998 relationship to the checkpoint signature
value. Authentication of the lake checkpoint signature, tiles, and inclusion or consistency proofs
belongs to liquid #207 and composes with this check.

## Lake transparency-log verification

`liquid::transparency` also verifies Lake transparency-log responses before a caller relies on
them. The coroutine-native `log_source` interface lets applications and tests provide checkpoints,
tiles, and proofs. `http_log_source` obtains Lake's HTTP representation without linking a Lake
library.

### HTTP URL validation (#289)

Use `http_log_source::create(url)` to construct a source through
`result<http_log_source>`. Malformed schemes, missing hosts, and ports outside the decimal
range 1–65535 return `errc::invalid_argument` with an HTTP/HTTPS URL diagnostic. Validation
is local: it does not resolve a host, open a socket, or request log data. Successful
construction also performs no I/O.

The string constructor remains source-compatible for existing clients. It retains a URL
validation error instead of throwing; each of its six request operations returns that same
error before entering the HTTP transport. New callers should use the factory to receive the
error at construction. This also protects existing admission and verification callers without
changing their proof or trust behavior.

Plain `http://` is accepted only when the host is loopback: `localhost`, an IPv4 address in
`127.0.0.0/8`, or `::1` (liquid #389). Anyone on the path of a plain-HTTP request can forge every
response, so any other `http://` URL is refused with an `invalid_argument` diagnostic naming the
loopback rule; use `https://`. Admission policies apply the same rule to `log.lake` when they load.

`log scan`, `log consistency`, `log verify`, and `log trust` use the factory before opening a
repository or decoding an attestation. Invalid URLs print the specific correction and exit **2** (invalid
usage); a valid URL's failed operation still exits **1**, as the
[command reference](../reference/cli/log.md#exit-status) specifies.

`HttpLogSource` API tests cover rejected schemes, absent hosts, empty/nondecimal/out-of-range
ports, valid HTTP/HTTPS URLs, and every legacy request operation. `cli/log-url.cmake` repeats
the invalid cases through all three verbs outside a repository and runs the API cases too.
On macOS and Linux, that CTest case uses the existing offline runner with networking denied.
No test in this regression starts a server or requires an endpoint.

A review of explicit throws in library-owned public constructors also found
`forge_client(std::string, forge_client_options)` in `src/forge.cpp` and
`sync_endpoint(std::string, sync_endpoint_options)` in `src/sync/endpoint.cpp` throwing
`std::invalid_argument` for invalid endpoints. Those remain outside #289. This review does
not establish that allocations or constructors in dependencies cannot throw.

### Checkpoints and proofs

The client validates the checkpoint's compact canonical JSON hash, then the Ed25519 Lake signature
over `TAAP/1 lake_checkpoint/1`, a NUL byte, and the CBOR checkpoint body. It accepts only
configured witnesses with valid signatures over the canonical JSON body. Duplicate witness keys do
not count twice, and too few distinct valid signatures fail with `checkpoint witness threshold not
met`.

Under [bridge decision 37](https://github.com/oramasearch/liquid-bridge/blob/main/docs/decisions.md),
the `lake_checkpoint/1` body can carry `issued_at`, a nonnegative integer of Unix epoch seconds,
UTC, covered by the same signature, canonical body hash, and witness signatures. Older bodies
omit it. The shared verifier authenticates the entire body without imposing an age policy;
[admission](admission.md#authenticated-checkpoint-age) requires this field when a positive
`max_checkpoint_age_seconds` is configured. The log attests its own clock; independent RFC 3161
time binding remains outside this admission policy change. Lake's producer update is lake#128.

Entry tiles use `LAKE-TILE-1\n` followed by nonzero big-endian lengths and canonical TAAP
attestations. Hash tiles use `LAKE-HASH-TILE-1\n` followed by 32-byte hashes. Every tile hash is
validated, every leaf is recomputed as `SHA256(0x00 || entry)`, and every parent as
`SHA256(0x01 || left || right)`. The verifier requests the exact full or partial tile at level zero,
then every non-empty parent level through the checkpoint. Each received parent value is compared
with the child pair that produced it before the split-tree root is compared with the checkpoint.
An error names the level, index, and width of the missing, malformed, or mismatched tile.

`log_client` keeps a per-client cache keyed by that exact coordinate. The cache stores immutable
tile bytes, never a trusted verdict. Every use decodes the cached bytes again, recomputes every leaf
and parent, and authenticates the complete walk to the selected checkpoint. A failed walk does not
add newly received tiles to the cache. A successful walk replaces the cache with the exact tile set
for that checkpoint, which drops superseded partial widths instead of retaining one version per log
append.

### Pinned log key and witness policy (#389)

Every verifying `log_client` operation (`current_checkpoint`, both `verify_inclusion` overloads,
`verify_consistency`, and `scan`) takes a caller-supplied `log_trust`: the log's Ed25519 key and
TAAP identifier, the witnesses whose cosignatures count, and the threshold. No operation reads the
source's `identity()` or `configuration()`, Lake's `/v1/identity` or
`/v1/transparency_log_configuration`. Before this change the repository-backed overloads, the
consistency check, and scans took the key and the threshold from those endpoints, so a malicious
log, or anyone on the path of a plain-HTTP log, could answer with its own key and
`witness_threshold: 0` and witness cosigning gave no split-view protection.

With the pin in place:

- A checkpoint signed under any other identifier is refused with `checkpoint is not signed by the
  pinned log key`; one signed by another key under the pinned identifier fails its signature check.
  A log that changes its key is never re-pinned automatically.
- The pinned threshold is the only one applied. The log's own threshold is not read, so a log cannot
  lower it.
- Only cosignatures from pinned witness keys count. A valid signature from any other witness,
  including one the log lists in its own configuration, is ignored.
- `validate_log_trust` refuses an unusable pin before any request: a key that is not 32 bytes under
  a `taap:1:ed25519:` identifier, a witness with no endpoint or a repeated key, or a threshold above
  the number of pinned witnesses. A threshold of zero is accepted only because it is written in the
  pin; it means no witness protection.

Library callers (Lake, liquify, admission policy sets) pass their own `log_trust`. The `liquid`
CLI and the repository workflows read it from the user's liquid home, `$LIQUID_HOME` or else
`$HOME/.liquid`, through `load_log_trust(url)`:

```text
$LIQUID_HOME/trust/transparency/<name>
```

`<name>` is `log_trust_file_name(url)`: the URL without `https://` (or with `http://` replaced by
`http-`), without trailing slashes, and with every character other than ASCII letters, digits, `.`
and `-` replaced by `_`. `https://lake.example:8443/log` is `lake.example_8443_log`. A name over 160
characters keeps its first 160 and adds `-` and 16 hexadecimal digits of the URL's SHA-256. Nothing
under `trust/` is ever read from a repository, a clone, a sync peer, or a log. A missing file is
`errc::not_configured`, and the message names the file to create: verification fails closed.

The file is a small line format, like the other files under `trust/`. Blank lines and lines starting
with `#` are ignored; each other line is a directive and space-separated values:

```text
# liquid transparency-log pin (liquid#389).
version liquid_log_pin/1
log https://lake.example
key taap:1:ed25519:<id> <64 lowercase hex digits of the Ed25519 public key>
witness https://witness-1.example/checkpoint <64 lowercase hex digits>
witness https://witness-2.example/checkpoint <64 lowercase hex digits>
threshold 2
```

`version`, `log`, `key`, and `threshold` appear exactly once; `witness` appears once per pinned
witness. The `log` line must name the URL being verified, so a pin copied to another log's file is
refused. Unknown directives, repeated lines, and files over 64 KiB are refused.
`parse_log_trust` and `format_log_trust` read and write this format; `save_log_trust` creates a new
file and refuses to replace an existing one (`errc::already_exists`).

`liquid log trust <log-url>` is the explicit trust-on-first-use step, and nothing runs it
implicitly. It refuses when a pin already exists. Otherwise it reads what the log says its key,
witnesses, and threshold are, checks that the current checkpoint verifies under them, prints the
pin file it would write and `fingerprint=sha256:<hex>` (the SHA-256 of that text), and pins
nothing. After checking the key and witnesses with the log's operator, run
`liquid log trust <log-url> --confirm <fingerprint>`: it reads the log again and writes the pin only
if the text has exactly that fingerprint. A log that reports a threshold of zero gets a warning that
such a pin gives no split-view protection. To change a pin, edit or remove its file; a log cannot.

An inclusion proof must bind the supplied exact canonical TAAP bytes, leaf index, tree size, root,
and checkpoint. Before a consistency proof is requested or a bootstrap checkpoint is stored, the
same complete tile walk authenticates the selected checkpoint. The consistency proof then binds
both the stored and new checkpoints and proves an append-only extension. Identity scans validate
all tiles to the current checkpoint and return every canonical TAAP attestation naming the
requested identity.

The most recent accepted checkpoint is a canonical-CBOR repository metadata record under a
SHA-256-derived log name, not a separate database. On first use, operators can pin a checkpoint
with `bootstrap_policy::pinned`; `accept_with_warning` returns a recorded bootstrap warning.

Each command below loads the log's pin first and refuses when there is none.
`liquid log verify <log-url> <attestation-base64url>` verifies inclusion and records its checkpoint.
`liquid log consistency <log-url>` advances trusted state only through a consistency proof.
`liquid log scan <log-url> <identity>` returns matching canonical attestations. `liquid verify
--log <log-url>` requires inclusion for each agreeing reproducibility attestation. For every selected
action, run `liquid verify --all --log <log-url>`. `liquid log
timestamps <log-url>` and `liquid log verify --timestamps` retain the RFC 3161/RFC 4998 commands.

`cli-verify-log` exercises that policy through the CLI handler. It stores a signed, agreeing
`reproducibility/1` attestation in a temporary repository and serves the matching checkpoint, entry
tile, and inclusion proof over loopback HTTP. It requires the unpinned log to be refused, then pins
it and requires verification to pass. It also requires a pin whose witness threshold is higher than
the one the log reports, and missing, tampered, and unavailable inclusion, to fail; without `--log`, verification retains its local-only policy. The test injects
only the already-completed verification report, so it can cover the CLI policy without a sandbox
mount. `cli-verify-log-guide` reads the documented all-actions invocation and runs it through the
built command parser. The typed command table remains covered independently by generated help and
reference tests.

`tests/fixtures/transparency/fixture.json` is copied from Lake commit `cb60ed4` and is generated
by Lake's `tests/fixtures/transparency/regenerate.py`. The tests use its checkpoint, tiles, and
proofs and cover modified, missing, reordered, substituted, forged, cached, and parent-only corrupt
data. The consistency regression removes the middle hash level and proves that verification fails
before requesting its otherwise valid proof.

The #389 cases in `test_lake_transparency.cpp` give the fixture source a hostile voice: it can
report a different identity or configuration, and it counts every read of them. A log that drops
its witness signatures and reports threshold zero is refused by `current_checkpoint`,
`verify_consistency`, and `scan`; a checkpoint re-signed by another key, under a new identifier or
the pinned one, is refused; a valid cosignature from a witness outside the pinned set does not
count toward the threshold; a pin that cannot be met is refused before any request; and none of
them reads the log's identity or configuration. `transparency_pin` cases round-trip the pin file,
refuse malformed and mismatched pins, refuse a missing pin with the file named, and refuse to
replace an existing pin.

## Tile-walk measurement

`liquid-transparency-measure` constructs a 65,536-leaf (`2^16`) log in memory before timing, then
verifies it once from its source and once from the client's byte cache. The timed region includes
tile lookup and copies, decoding, SHA-256 of every tile, leaf, parent, and split-tree root, and all
parent comparisons. It excludes construction of the synthetic log, checkpoint signatures, network
transport, and inclusion or consistency proof verification.

Run a Release build with:

```sh
cmake --build build-issue83-measure --target liquid-transparency-measure
build-issue83-measure/tests/liquid-transparency-measure
```

Five runs on 2026-09-06 used an Apple M3 Max with 64 GiB, macOS 14.6.1, and Homebrew Clang 22.1.8.
The median cold walk was 148,012 microseconds and the median cached walk was 149,409 microseconds.
Each cold walk read 137 exact tiles and 2,885,561 encoded bytes; each cached walk made zero source
reads but repeated the full parent and checkpoint authentication. Host load was
`198.13 201.58 132.04`, so these numbers record the accepted work and cache behavior rather than an
idle-host throughput baseline.
