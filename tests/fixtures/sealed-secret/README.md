# Sealed-secret v1 fixture

This directory is the cross-language fixture for liquid's sealed-secret v1 envelope. Hex files are
lowercase hexadecimal with one trailing newline; decode the text before using the bytes. The key id
is `lake-key-2026-09`. `scope.json` records the three authenticated scope strings.

`recipient-private.hex` and `recipient-public.hex` are the fixed X25519 recipient keypair.
`ephemeral-private.hex` is the deterministic test input that produces `ephemeral-public.hex`.
`nonce.hex`, `plaintext.hex`, and `record.cbor.hex` are the remaining fixed inputs and exact output.
The ephemeral private key is test material only and is not part of the CBOR record.

Regenerate the values after a deliberate wire-format change:

```sh
cmake --build build --target liquid-secret-fixture-generator
build/tests/liquid-secret-fixture-generator
```

The C++ fixture test decodes and unseals `record.cbor.hex`, then injects the decoded ephemeral
private key and nonce in place of the operating-system CSPRNG and requires byte-identical output.
The operator's Go test consumes the same files using `curve25519.X25519`,
`hkdf.New(sha512.New, ...)`, and `chacha20poly1305.NewX`.
