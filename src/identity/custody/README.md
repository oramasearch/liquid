# KEK custody providers

Each file implements one private `key_provider` kind. `common.cpp` contains encoding,
SHA-256/HMAC/HKDF, RFC ChaCha20-Poly1305, JSON, environment, and error helpers. `http.cpp` is the
Capy/Corosio client over `oramasearch/http`; remote providers perform no blocking wait inside a
coroutine.

`age.cpp` implements the age v1 X25519 recipient form. `aws_kms.cpp`, `gcp_kms.cpp`, and
`azure_kv.cpp` implement the vendors' JSON APIs without SDK dependencies. `lake.cpp` implements the
lake-native transit contract. `shamir.cpp` splits and reconstructs the local random KEK over
GF(256). Provider credentials are read at use time from the environment and never enter v3
parameters.

`keychain.cpp` implements `keychain:<account>` on macOS: a random 32-byte KEK stored as a generic
password (service `dev.liquid.custody`, the URI's account) in the login Keychain through
Security.framework, created on first wrap, never synchronised; data keys are sealed under it with
XChaCha20-Poly1305 bound to the wrap context. Other platforms return `errc::unsupported`. The local
identity uses it for its wrapping key by default on macOS (liquid #402, `docs/library/identity.md`).

`tpm.cpp` implements `tpm:pcrs=<list>` sealing through the TPM 2.0 command subset. Linux submits
through `/dev/tpmrm0`, Windows through TBS, and macOS returns an explicit unsupported result.
Blocking device calls run under `runtime::offload`; the transport can be replaced by the protocol
double in tests.
