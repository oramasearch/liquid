# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options and repeatable named result/tree inputs, calls `liquid::build::wrap`, and prints the
canonical output tree, action and cache counts, per-tier action and byte counts, phase attribution,
the critical path, record-derived miss details for `--explain`, and incompatible library reports.
It applies `--fail-on-abi-break` after a successful build.
`trace.cpp` implements profiled build invocation, Chrome trace export, and `why-slow` entirely
through the public observability API. Text and Chrome trace projections include the recorded
execution decision and every cost-model input. `why.cpp` implements `liquid why` and `liquid cache
stats` through the public build API, including scheduling fields, policy-refusal causes, and their
JSON forms. `wrap.cpp` also
parses `--target`, prints the probe-start count, and exposes foreign-target errors from the library.
`facts.cpp` implements list, show, and bundle export entirely through public build APIs. The commands
never read the private action cache directly.

`attestation.cpp` also parses `attest image`, delegates statement minting to libliquid, and prints
the stored digest. `serve.cpp` parses admission policy, listener, TLS, source, and cache settings;
the long-running webhook remains a public library operation.

`trace.cpp` also prints the configured public plan for `build --plan`, starts `build [target]`
through the daemon, and consumes `build_handle` for both repository and `build --events` streams,
printing the stable JSON projection one event per line. `test.cpp` opens the wrapped build repository
through the public repository API, runs an optional test filter, and prints each streamed test result.

`repo.cpp` implements the scripted `write` command's base, cursor, dependency-path, and stale
policy parsing through public repository APIs. Status projects persisted agent policy, and
`log --ops --json` projects signed drip bases and re-validation results without reading repository
storage directly.

`anchor.cpp` parses `<path>:<line>@<change> --to <change>`, calls the public text-anchor mapper,
and prints the target coordinate plus moved, approximate, and deleted state. It does not inspect
repository storage or file contents.

`diff.cpp` resolves each operand as a ref or change digest and calls public repository diff.
`--text` opts into line hunks, `--formats` prints codec-tier container trees, an optional path
filters the result, and `--json` emits the same nested values for tools.

`retain.cpp` parses `retain explain <digest>`, opens the current repository, and prints the public
`repository::retain_explain` result. Rule evaluation and reachability stay in libliquid.
`verify.cpp` parses action, all, changed, and graph selectors, calls the public verification APIs,
prints action attestations or graph audit records, names critical query inputs and output
differences, and returns failure on divergence.
The same command group implements `replay`: it parses an action or result plus `--json` and
`--record`, calls the public replay API, and treats a matching recorded nonzero exit as success.

`provenance.cpp` implements both `provenance` and the artifact-oriented `verify` alias entirely
through public build APIs. It verifies before writing native JSON or the requested DSSE projection.
`sbom.cpp` selects SPDX or CycloneDX, resolves a graph-derived document through the public build
API, and uses the public foreign-binary path only when no producing result exists.
`abi.cpp` resolves regular files, blob digests, output paths, and exact result/output pairs through
the public ABI and repository APIs. It prints stable text or JSON descriptors and comparisons.

`policy.cpp` implements `policy check` and `policy explain` through public build APIs. It prints
canonical rule configuration and per-input verified-identity counts without reading the store
directly.

`delta.cpp` implements create, digest-checked apply, and fetch through the public delta API. A
fetch can hydrate a patch or target registered with the store's configured remote sources.

`keys.cpp` implements custody inspection, rewrap, data-key rotation, version revocation, and
Shamir splitting entirely through the public keyring API. Credentials remain in provider-specific
environment variables; command arguments contain only public custody URIs.

`search.cpp` starts or attaches to the checkout daemon, parses optional ref, result-limit, and
`--explain` selectors, and calls `repository::search` or `repository::plan_search`. Results include
optional score and located path coordinates. Explain mode prints stable JSON with chosen and
alternative costs plus every planner input. It does not read the index files directly.

`resolve.cpp` parses root specs and universe, reuse, explanation, and JSON selectors. It calls only
the public package-universe loader, resolver, and explanation projections. Text graphs are indented
by dependency; conflict and package-choice explanations use the stable library formats.

`sync.cpp` implements `push` and `pull` through the public repository and sync APIs. It passes an
optional `LIQUID_TOKEN` and push `--atomic` choice to the endpoint, prints stable metadata, object,
byte, cursor, and per-ref outcome lines, and returns failure when any ref is refused.
`review.cpp` implements lake review listing, display, creation, anchored comments, state updates,
branch protections, and the polling watch entirely through `forge_client`. It reads `LIQUID_TOKEN`
for the bearer and pins both line-oriented text and JSON output without exposing the token.
`secret.cpp` fetches Lake sealing keys, seals stdin or file values, emits canonical CBOR or base64,
and performs local break-glass unsealing through the public secret API. Private-key files must be
32 raw bytes with mode `0600` on POSIX systems; key and value bytes are never written to errors.
`serve.cpp` implements `serve sync` with the public `sync_server` and the vendored
`oramasearch/http` router. Its repeatable `--allow-ref` glob installs the public reference
authorization callback and returns one verdict per update. Request handlers await library tasks on
the listener executor and never include the private protocol codec.

The CLI-internal metadata and validator live in `metadata.hpp` and `arguments.cpp`. Every command
passes through the typed option allow-list before its handler interprets values. `docs.cpp` renders
the same table as command help, Markdown, stable JSON, and man pages. The generated reference lives
in [`docs/reference/cli/`](../../docs/reference/cli/); the replay output contract and contributor
notes remain in [`docs/cli/README.md`](../../docs/cli/README.md).
The same file maps `serve admission` flags and environment variables to the public admission
options. The library owns policy loading, TLS, review decisions, control routes, and metrics; the
CLI only prints listener state and the explicit insecure-HTTP warning.
`drive.cpp` configures or reopens a public `drive_client`, prints the persisted three-tree and
per-reference state, and attaches `--watch` to the checkout daemon after the first successful
one-shot reconciliation. `history.cpp` lists node-following path versions and restores one as an
ordinary repository drip through the public repository API.

The public command table, replay output contract, and user-facing examples live in
[`docs/cli/README.md`](../../docs/cli/README.md).

`history.cpp` implements `ops` through the public operation-log API. Its JSON form includes the
operation session. `repo.cpp` implements `log --ops` through the signed drip API. Undo accepts one
operation ID or a session with
repeatable path and drip filters, and renders every applied or skipped report entry.

`workers.cpp` implements the scheduler and worker daemons with flag-over-environment
configuration. Long-running entry points run on the guarded process-wide runtime while the command
thread waits for completion, keeping accept and polling loops live between I/O callbacks.
`node_store.cpp` implements `node-store serve` plus the status, mount, unmount, and prefetch thin
HTTP clients. Flags override the documented node-store environment variables.
`wrap.cpp` selects the scheduler backend with `--scheduler` while `--remote` keeps the stock REAPI
path.

Repository status includes bytes still remote in both text and JSON. Build explanation renderers
also name the non-hermetic-read reason and its path for remote-eligibility diagnostics.
Status also projects daemon graph-validation backlog, flow watermarks and buckets, and the last
divergence in text and JSON. Push prints the peer's exhausted scope and retry delay on HTTP 429.
Wrap output includes its graph backlog and last divergence record.

`symbols.cpp` detects the language from the public registry's extension table, uses the compiler
frontend when it supports that language, falls back to the locked tree-sitter grammar, and presents
the public symbol record without inspecting private repository storage. `version --json` includes
the canonical grammar-lock digest alongside the release version.

`hot_update.cpp` classifies two recorded builds from public hot-update evidence and prints the
tier, outcome, symbols, and evidence records. Its status view verifies every signed patch before
showing the base artifact plus its patch composition.
