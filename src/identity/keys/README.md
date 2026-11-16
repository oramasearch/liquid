# Identity key backends

`backend.hpp` is the private signing-key boundary. `keys.cpp` retains the software Ed25519 path,
owns CTAP2 make-credential/get-assertion CBOR for FIDO2, and owns PIV selection, P-256 generation in
slot `9c`, and General Authenticate APDUs. Blocking HID and PC/SC calls run under
`runtime::offload`; tests install transports that consume the same command bytes.

`key_backend` exposes public-key metadata, an opaque persistence descriptor, and asynchronous
signing. Persisted hardware descriptors contain custody, algorithm, credential or slot identifier,
and public key. They never contain a hardware private key.

No vendor hardware library crosses this boundary. Operating-system HID and PC/SC adapters supply
production transports, while TAAP verification remains software-only. A FIDO2 assertion is
verified over `authenticatorData || clientDataHash`, the bytes CTAP2 authenticators sign
([`docs/library/identity.md`](../../../docs/library/identity.md)).
