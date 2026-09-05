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
