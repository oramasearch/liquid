# Fuzz harnesses

libFuzzer harnesses for the parsers that read untrusted bytes (liquid
[#407](https://github.com/oramasearch/liquid/issues/407)). They are built only with
`-DLIQUID_FUZZ=ON`, which also compiles every C and C++ target, dependencies included, with
AddressSanitizer, UndefinedBehaviorSanitizer and coverage instrumentation
([`cmake/Hardening.cmake`](../cmake/Hardening.cmake)). How to build and run them, locally and on the
Linux BuildKit lane, and the recorded runs: [`docs/build/hardening.md`](../docs/build/hardening.md).

| File | What it fuzzes |
|---|---|
| [`fuzz.hpp`](fuzz.hpp) | Shared helpers: the selector byte that picks a decoder, two-byte length-prefixed parts, and `require` for checked properties. |
| [`cbor.cpp`](cbor.cpp) | Canonical CBOR decode, re-encode and decode again (`src/encoding/cbor.cpp`). |
| [`sync.cpp`](sync.cpp) | The sync wire framing: metadata document, object list, pack frames (`src/sync/codec.cpp`). |
| [`objects.cpp`](objects.cpp) | Object records: blob, tree, change, conflict, repository, attestation, action, result, action index, build trace, SBOM, ABI, execution schedule (`src/objects/records.cpp`). |
| [`drip.cpp`](drip.cpp) | Drips: the journal record (`crdt::decode_drip_record`) and the signed drip body history verification reads. |
| [`identity.cpp`](identity.cpp) | The identity chain and signature envelope: identity documents with signature verification, signatures, chains across rotations, scan attestations. |
| [`http1.cpp`](http1.cpp) | HTTP/1.1: the Boost.HTTP request parser as liquid's servers configure it, fed in pieces, and the client's response parser with chunked decoding. |
| [`http2.cpp`](http2.cpp) | HTTP/2 frames, HPACK blocks across one dynamic table, Huffman strings, gRPC message framing and status. |
| [`configuration.cpp`](configuration.cpp) | The manifest: `.liquid` parse, check, format and lower (with its `locks` rules), and the stored target, source and install-intent records. |
| [`resolve.cpp`](resolve.cpp) | Package-manager lock records: package specs, package records, concretised specs. |
| [`git.cpp`](git.cpp) | Git objects: loose object headers with the identifier check, commit and tree payloads, pack deltas. |
| [`protobuf.cpp`](protobuf.cpp) | The protobuf wire reader and the REAPI v2 messages a remote execution peer sends. |
| [`seeds.cpp`](seeds.cpp) | `liquid-fuzz-seeds`, which writes the seed corpus from the test fixtures and from liquid's own encoders. |

Each harness is the executable `build/fuzz/liquid-fuzz-<name>`; `cmake --build build --target
liquid-fuzz` builds them all and writes the corpus to `build/fuzz/corpus/<name>/`.
