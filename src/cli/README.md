# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, per-tier action and byte
counts, phase attribution, the critical path, and record-derived miss details for `--explain`.
`trace.cpp` implements profiled build invocation, Chrome trace export, and `why-slow` entirely
through the public observability API. Text and Chrome trace projections include the recorded
execution decision and every cost-model input. `why.cpp` implements `liquid why` and `liquid cache
stats` through the public build API, including scheduling fields, policy-refusal causes, and their
JSON forms. `wrap.cpp` also
parses `--target`, prints the probe-start count, and exposes foreign-target errors from the library.
`facts.cpp` implements list, show, and bundle export entirely through public build APIs. The commands
never read the private action cache directly.

`retain.cpp` parses `retain explain <digest>`, opens the current repository, and prints the public
`repository::retain_explain` result. Rule evaluation and reachability stay in libliquid.
`verify.cpp` parses action, all, and changed selectors, calls the public verification API, prints
attestation outcomes and critical output differences, and returns failure on divergence.
The same command group implements `replay`: it parses an action or result plus `--json` and
`--record`, calls the public replay API, and treats a matching recorded nonzero exit as success.

`provenance.cpp` implements both `provenance` and the artifact-oriented `verify` alias entirely
through public build APIs. It verifies before writing native JSON or the requested DSSE projection.
`sbom.cpp` selects SPDX or CycloneDX, resolves a graph-derived document through the public build
API, and uses the public foreign-binary path only when no producing result exists.

`policy.cpp` implements `policy check` and `policy explain` through public build APIs. It prints
canonical rule configuration and per-input verified-identity counts without reading the store
directly.

`delta.cpp` implements create, digest-checked apply, and fetch through the public delta API. A
fetch can hydrate a patch or target registered with the store's configured remote sources.

`sync.cpp` implements `push` and `pull` through the public repository and sync APIs. It passes an
optional `LIQUID_TOKEN` to the endpoint and prints stable metadata, object, byte, and cursor counts.
`serve.cpp` implements `serve sync` with the public `sync_server` and the vendored
`oramasearch/http` router. Request handlers await library tasks on the listener executor and never
include the private protocol codec.

The public command table, replay output contract, and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).

Repository status includes bytes still remote in both text and JSON. Build explanation renderers
also name the non-hermetic-read reason and its path for remote-eligibility diagnostics.
