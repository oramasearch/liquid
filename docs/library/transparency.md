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
