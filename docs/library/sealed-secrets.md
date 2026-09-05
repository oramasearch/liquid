# Sealed secrets

The `sealed_secret` record carries a value to one Lake environment without giving the sender a
decryption key. It is liquid's own versioned envelope; it is not compatible with kubeseal.

## Version 1 record

The serialized form is this canonical-CBOR map, with exactly these fields:

```text
{
  v: 1,
  kid: text,
  epk: bytes(32),
  nonce: bytes(24),
  aad: {
    cluster: text,
    namespace?: text,
    name?: text
  },
  ct: bytes
}
```

`cluster` is required. `namespace` and `name` are independently optional. The decoded scope must
equal the expected scope exactly: a different value, or a field present on only one side, is an
authorization failure naming `cluster`, `namespace`, or `name`. The top-level map has six fields;
`aad` has no fields other than the three above. Empty `kid` and `cluster` values are invalid.

All text is UTF-8 CBOR text. `epk`, `nonce`, and `ct` are CBOR byte strings, not encoded text.
`ct` is the XChaCha20-Poly1305 ciphertext followed by its 16-byte tag. Decoding rejects any input
whose re-encoding is not byte-identical canonical CBOR with `errc::corrupt`.

The authenticated data is the canonical CBOR encoding of the same record with `ct` omitted:

```text
{ v: 1, kid: kid, epk: epk, nonce: nonce, aad: scope }
```

Consequently the version, key identifier, ephemeral key, nonce, and every present scope field are
authenticated. Changing any of them causes authentication to fail before plaintext is returned.

## Key agreement and encryption

The recipient publishes an opaque `kid` and a 32-byte X25519 public key `pk`. liquid never derives
or interprets `kid`. Each seal performs these operations:

1. Draw a fresh 32-byte ephemeral private key `esk` from the operating-system CSPRNG and compute
   `epk = X25519-public(esk)`.
2. Compute `ss = X25519(esk, pk)` and reject an all-zero result.
3. Set `salt = epk || pk`, exactly 64 bytes in that order.
4. Set `info = UTF8("liquid sealed secret v1") || 0x00 || UTF8(kid)`.
5. Derive 32 bytes with HKDF-SHA-512 using `salt`, `ss` as input key material, and `info`.
6. Draw a fresh 24-byte nonce from the operating-system CSPRNG.
7. Encrypt with XChaCha20-Poly1305 using the derived key, nonce, canonical authenticated header,
   and plaintext.

The ephemeral private key, shared secret, and derived key are wiped before sealing returns.
Unsealing derives `pk` from the recipient private key, repeats the agreement and KDF, verifies the
exact expected scope, and releases plaintext only after the authentication tag succeeds. A wrong
private key, invalid key agreement, header change, or ciphertext change returns
`errc::unauthorized`; no partial plaintext is returned.

The implementation uses the pinned Monocypher X25519, HKDF-SHA-512, and XChaCha20-Poly1305
functions already compiled into libliquid. It adds no crypto dependency.

## Go mapping

The operator-side implementation uses only `golang.org/x/crypto` in addition to the Go standard
library:

- `curve25519.X25519(privateKey, epk)` for the shared secret;
- `hkdf.New(sha512.New, ss, epk || pk, info)` and a 32-byte read for the key; and
- `chacha20poly1305.NewX(key)`, then `Open(nil, nonce, ct, authenticatedHeader)`.

Go's `Seal` output is ciphertext followed by the tag, matching `ct`. The operator must reject an
all-zero X25519 result, reconstruct the authenticated header canonically, and compare the record's
scope to its own cluster, namespace, and object name before returning plaintext.

## Visibility and custody

An author can see the source value while sealing it and may retain whatever source supplied it.
Possession of the envelope and cluster public key does not permit decryption. The `kid`, ephemeral
public key, nonce, cluster, namespace, name, ciphertext length, and record version are visible to
anyone who can read the record; scope is authenticated metadata, not confidential metadata. The
cluster private key remains in the operator's `lake-<name>-sealing-key` custody and is never sent to
liquid or Lake. The operator can recover the plaintext while reconciling the target secret.

Fresh ephemeral and nonce bytes make two envelopes for the same value different. The record carries
`kid` so the operator can select a retained cluster key during rotation; rotation policy is outside
this format.

## Worked fixture

The interoperability fixture is in
[`tests/fixtures/sealed-secret/`](../../tests/fixtures/sealed-secret/). Its inputs are:

```text
kid = lake-key-2026-09
cluster = lake-prod
namespace = payments
name = database
recipient private = 3031323334353637383961626364656630313233343536373839616263646566
recipient public  = 86517bc0c39fe876b7646cb5e34ee03daf71ccbad67450c16a970ad8cbc03e7e
ephemeral private = 6665646362613938373635343332313066656463626139383736353433323130
ephemeral public  = dbfca07744d1aa20b5b1e6ebe9568ccf15309ea27d666a3df07c09a680938512
nonce             = 6e6f6e63652d666f722d6c69717569642d66697874757265
plaintext         = 636f727265637420686f727365206261747465727920737461706c650a
```

The exact canonical record is:

```text
a6617601626374582d3cc26741a495ae2a371c30c7aab293928a12c56d82a09149fd459b4f3fd3bd5dd9590b74da99abe4d6e1de226e63616164a3646e616d6568646174616261736567636c7573746572696c616b652d70726f64696e616d657370616365687061796d656e74736365706b5820dbfca07744d1aa20b5b1e6ebe9568ccf15309ea27d666a3df07c09a680938512636b6964706c616b652d6b65792d323032362d3039656e6f6e636558186e6f6e63652d666f722d6c69717569642d66697874757265
```

To verify another implementation, decode `record.cbor.hex`, assert each field against the fixture,
unseal it to `plaintext.hex`, then replace its random source with the concatenation of
`ephemeral-private.hex` and `nonce.hex`. Sealing the fixture plaintext and scope must reproduce
`record.cbor.hex` byte for byte. The deterministic source is test-only and is not exposed by the
public API.

## Lake capability

`fetch_sealing_public_key` reads `GET <lake>/v1/sealing_public_key` through liquid's private HTTP
client and optionally sends a bearer token. The successful Lake envelope is:

```json
{
  "ok": true,
  "value": {
    "kid": "lake-key-2026-09",
    "public_key": "86517bc0c39fe876b7646cb5e34ee03daf71ccbad67450c16a970ad8cbc03e7e",
    "algorithm": "x25519"
  },
  "duration_ms": 1
}
```

The key is exactly 64 hexadecimal characters and `algorithm` must be `x25519`. Standard Lake error
envelopes retain their invalid-input, authorization, not-found, conflict, and unavailable meaning.
