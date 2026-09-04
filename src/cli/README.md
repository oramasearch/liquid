# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, per-tier action and byte
counts, and record-derived miss details for `--explain`. `why.cpp` implements `liquid why` and
`liquid cache stats` through the public build API, including their JSON forms. The commands never
read the private action cache directly.
`verify.cpp` parses action, all, and changed selectors, calls the public verification API, prints
attestation outcomes and critical output differences, and returns failure on divergence.

`provenance.cpp` implements both `provenance` and the artifact-oriented `verify` alias entirely
through public build APIs. It verifies before writing native JSON or the requested DSSE projection.

The public command table and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).
