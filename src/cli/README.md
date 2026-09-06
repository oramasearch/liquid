# Command-line implementation

Each source file maps one command group to public libliquid calls. `wrap.cpp` parses build-wrapper
options, calls `liquid::build::wrap`, prints action and cache counts, per-tier action and byte
counts, phase attribution, the critical path, and record-derived miss details for `--explain`.
It also prints incompatible library reports and applies `--fail-on-abi-break` after a successful
build.
`trace.cpp` implements profiled build invocation, Chrome trace export, and `why-slow` entirely
through the public observability API. Text and Chrome trace projections include the recorded
execution decision and every cost-model input. `why.cpp` implements `liquid why` and `liquid cache
stats` through the public build API, including scheduling fields, policy-refusal causes, and their
JSON forms. `wrap.cpp` also
parses `--target`, prints the probe-start count, and exposes foreign-target errors from the library.
`facts.cpp` implements list, show, and bundle export entirely through public build APIs. The commands
never read the private action cache directly.

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

`search.cpp` starts or attaches to the checkout daemon, parses optional ref, result-limit, and
`--explain` selectors, and calls `repository::search` or `repository::plan_search`. Results include
optional score and located path coordinates. Explain mode prints stable JSON with chosen and
alternative costs plus every planner input. It does not read the index files directly.

`sync.cpp` implements `push` and `pull` through the public repository and sync APIs. It passes an
optional `LIQUID_TOKEN` and push `--atomic` choice to the endpoint, prints stable metadata, object,
byte, cursor, and per-ref outcome lines, and returns failure when any ref is refused.
`serve.cpp` implements `serve sync` with the public `sync_server` and the vendored
`oramasearch/http` router. Its repeatable `--allow-ref` glob installs the public reference
authorization callback and returns one verdict per update. Request handlers await library tasks on
the listener executor and never include the private protocol codec.

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
Status also projects daemon graph-validation backlog and the last divergence in text and JSON.
Wrap output includes its graph backlog and last divergence record.

`symbols.cpp` loads the compiler frontend when installed, falls back to tree-sitter, and presents
the public symbol record without inspecting private repository storage.
