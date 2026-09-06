# Cryptography internals

This directory contains libliquid's private cryptographic implementation. Public value types and
verification entry points live under `include/liquid/`; private arithmetic and parsing helpers do
not cross that boundary.

`crypto.cpp` contains the existing Monocypher-backed TAAP operations. `signature.cpp` adds
verification-only SHA-384, RSA PKCS#1 v1.5, RSA-PSS, and ECDSA P-256/P-384 operations. It contains
no key generation or signing entry point.
