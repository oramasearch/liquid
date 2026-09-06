# Sealed-secret implementation

`sealed_secret.cpp` implements the public sealed-secret value types, canonical-CBOR codec, X25519
and HKDF-SHA-512 key schedule, XChaCha20-Poly1305 encryption, exact scope validation, and lake
public-key fetch. Pure record and crypto operations are synchronous. Fetching uses the shared
private HTTP client and returns a Capy task.

Tests inject a bounded deterministic byte source through `secret.hpp`; production calls always use
the operating-system CSPRNG in `src/crypto/crypto.cpp`.
