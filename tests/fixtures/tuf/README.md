# Lake TUF fixture

These are the exact compact bytes stored by Lake's production
`lake::transparency::Service` at Lake revision
`964cca133c379b4cb3cb465953c7d74506dc69d8` after initializing root version 1 and publishing
targets, snapshot, and timestamp version 1. `entry.cbor` is the complete target named by
`1.targets.json`.

The fixture was generated once with a temporary C++ harness that compiled Lake's
`src/transparency/tuf.cpp`, reused the deterministic Ed25519 seed markers from
`TransparencyTuf.AppendsThresholdSignedMetadataAndRejectsRepositoryAttacks`, called
`Service::initialize` and `Service::publish`, and copied the four immutable files from
`Service::directory()`. The harness was run from this liquid worktree as:

```sh
LAKE_WORKTREE=/path/to/lake
clang++ -std=c++23 tuf_fixture_generator.cpp \
  "$LAKE_WORKTREE/src/transparency/tuf.cpp" \
  -I"$LAKE_WORKTREE/include" \
  -I"$LAKE_WORKTREE/modules/liquid/modules/automerge-cpp/upstream/json/include" \
  -I/opt/homebrew/opt/openssl@3/include -L/opt/homebrew/opt/openssl@3/lib -lcrypto \
  -o tuf-fixture-generator
./tuf-fixture-generator tests/fixtures/tuf
```

The checked bytes are:

| File | Bytes | SHA-256 |
|---|---:|---|
| `1.root.json` | 2,781 | `eb04448265ea623b4fdd46fe5d96ef21141d094191ac8cdaf1a481a5c6e3773d` |
| `1.targets.json` | 774 | `3a269db383b9a72973ecd1089ab5a348247e3f0dcb22dd5960cf0746d1a59552` |
| `1.snapshot.json` | 467 | `028d3cd8e1fbb3d26384d205ff923ccf1898f6cba22a386bbed6fb663741d6e1` |
| `1.timestamp.json` | 469 | `563ebf7b2907773fdfc1992fc60822e7d655db2b2ae9d988fc6c1b03ab6bea74` |
| `entry.cbor` | 4 | `24363a55ea25b40c87b8304e45a55f552f7a7f411c543065f415a21da01ce2b4` |

Attack metadata is built in `tests/test_tuf.cpp` so each changed signature, binding, expiry, or
version is visible beside the assertion that rejects it.
