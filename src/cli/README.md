# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, and prints changed manifest
paths for `--explain`. The command never reads the private action cache directly.
`verify.cpp` parses action, all, and changed selectors, calls the public verification API, prints
attestation outcomes and critical output differences, and returns failure on divergence.

The public command table and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).
