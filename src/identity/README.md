# Identity implementation

`taap.cpp` and `taap.hpp` hold canonical TAAP helpers. `key_version.hpp` is the shared parser and
formatter for data-key references. The public identity, signing, wrapped-key file, and keyring
implementation remains in `src/identity.cpp`.

`custody/` contains private provider implementations. Public callers use only
`liquid/identity.hpp`; vendor SDK types and provider transport details do not cross that boundary.
`keys/` contains the private software, CTAP2/FIDO2, and PIV signing backend interface and command
codecs. `threshold.cpp` evaluates signed release policy and approval records by distinct TAAP
identity.
