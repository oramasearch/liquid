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
