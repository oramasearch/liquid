# Fuzz harnesses

libFuzzer harnesses for the parsers that read untrusted bytes (liquid
[#407](https://github.com/oramasearch/liquid/issues/407), and the remaining parsers in
[#463](https://github.com/oramasearch/liquid/issues/463)). They are built only with
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
| [`store.cpp`](store.cpp) | The store's pack files (frame scan, sealed table, each frame read back through `read_pack_entry`) and the chunk index (manifest and runs), through an in-memory backend (`src/store/pack.cpp`, `src/store/index.cpp`). |
| [`gitpack.cpp`](gitpack.cpp) | Git pack files and version 2 pack indexes as `git_detail::object_store` reads them from disk: offsets, packed headers, `OFS_DELTA` and `REF_DELTA`, inflation (`src/git/object_store.cpp`). |
| [`delta.cpp`](delta.cpp) | liquid's binary delta: the delta record with a round-trip property, and zstd patches applied to a source (`src/delta.cpp`). |
| [`text.cpp`](text.cpp) | JSON with a round-trip property, and `.env` files (`src/formats/json.cpp`, `src/formats/dotenv.cpp`). |
| [`archive.cpp`](archive.cpp) | The container codecs: ZIP, OOXML, ODF, wheel, JAR, tar, deb, RPM, APK; `ar` members; the whole registry with nesting. |
| [`binary.cpp`](binary.cpp) | ELF, Mach-O and PE, as codecs and through `inspect_elf`, `inspect_macho`, `inspect_pe` (`src/formats/binary/`). |
| [`data.cpp`](data.cpp) | The HDF5, Parquet, SQLite and PDF codecs. |
| [`model.cpp`](model.cpp) | The Jupyter, safetensors and pickle codecs (`src/formats/model/`). |
| [`codec.hpp`](codec.hpp) | Shared driver for the four codec harnesses: link a built-in codec, then recognise, unpack with small limits, canonicalise, diff and repack. |
| [`der.cpp`](der.cpp) | DER elements, X.509 certificates, PEM and DER trust anchors, certificate paths, CMS SignedData (`src/asn1.cpp`, `src/x509.cpp`, `src/cms.cpp`). |
| [`secret.cpp`](secret.cpp) | Sealed secrets: the v1 record with a round-trip property, unsealing, and Lake's sealing-public-key response (`src/secret/`). |
| [`workers.cpp`](workers.cpp) | The scheduler's nine workers-protocol messages (`src/workers/protocol.cpp`). |
| [`tuf.cpp`](tuf.cpp) | TUF metadata: pinning a root, and updates with timestamp, snapshot, targets and rotated roots (`src/tuf.cpp`). |
| [`transparency.cpp`](transparency.cpp) | RFC 3161 timestamp tokens, RFC 4998 evidence records (`src/transparency.cpp`), and log entry and hash tiles. |
| [`memory_backend.hpp`](memory_backend.hpp) | An in-memory `store_backend` for the store harness and the seed generator. |
| [`seeds.cpp`](seeds.cpp) | `liquid-fuzz-seeds`, which writes the seed corpus from the test fixtures and from liquid's own encoders. |

automerge-cpp's own three fuzzers (`fuzz_load`, `fuzz_leb128`, `fuzz_change_chunk` in
`modules/automerge-cpp/fuzz/`) are built here as `liquid-fuzz-automerge-load`,
`liquid-fuzz-automerge-leb128` and `liquid-fuzz-automerge-change-chunk`: their own CMake file takes
the internal headers from `${CMAKE_SOURCE_DIR}`, which is liquid's root in a subproject, so
[`CMakeLists.txt`](CMakeLists.txt) compiles the same sources with the submodule's root as the
include directory, and the submodule is not changed.

Each harness is the executable `build/fuzz/liquid-fuzz-<name>`; `cmake --build build --target
liquid-fuzz` builds them all and writes the corpus to `build/fuzz/corpus/<name>/`.
