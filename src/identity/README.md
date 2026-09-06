# Identity implementation

`taap.cpp` and `taap.hpp` hold canonical TAAP helpers. `key_version.hpp` is the shared parser and
formatter for data-key references. The public identity, signing, wrapped-key file, and keyring
implementation remains in `src/identity.cpp`.

`custody/` contains private provider implementations. Public callers use only
`liquid/identity.hpp`; vendor SDK types and provider transport details do not cross that boundary.
