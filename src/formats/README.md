# Format codecs

Each built-in format owns one directory beneath this folder. Its implementation exposes a C ABI v1
descriptor to `builtin.hpp`; `registry.cpp` registers that descriptor through the same
`format_codec::linked` path used by statically linked external modules.

A codec must bound every size and offset before reading or allocating, honour the registry entry,
depth, and byte limits, and report unsupported objects explicitly. It may not add a format vendor's
library or expose third-party types. Synchronous codec work called from a coroutine runs through
`runtime::offload`.

Every new codec adds a real fixture and passes the common cases in `tests/formats/`: recognition,
unpack, canonicalisation, logical comparison, every truncation boundary, recursion limits, and a
byte-identical repack assertion whenever recognition reports Tier 1. Supported and unsupported
subsets belong in `docs/library/formats.md` in the same change.
