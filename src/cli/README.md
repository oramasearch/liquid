# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, per-tier action and byte
counts, phase attribution, the critical path, and record-derived miss details for `--explain`.
`trace.cpp` implements profiled build invocation, Chrome trace export, and `why-slow` entirely
through the public observability API. `why.cpp` implements `liquid why` and `liquid cache stats`
through the public build API, including policy-refusal causes and their JSON forms. `wrap.cpp` also
parses `--target`, prints the probe-start count, and exposes foreign-target errors from the library.
`facts.cpp` implements list, show, and bundle export entirely through public build APIs. The commands
never read the private action cache directly.
`verify.cpp` parses action, all, and changed selectors, calls the public verification API, prints
attestation outcomes and critical output differences, and returns failure on divergence.

`provenance.cpp` implements both `provenance` and the artifact-oriented `verify` alias entirely
through public build APIs. It verifies before writing native JSON or the requested DSSE projection.

`policy.cpp` implements `policy check` and `policy explain` through public build APIs. It prints
canonical rule configuration and per-input verified-identity counts without reading the store
directly.

The public command table and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).
