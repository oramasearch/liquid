# Build tests

`test_executor.cpp` exercises public action execution and the private disk-cache invariants. Tests
that require observation and sandbox enforcement run only when FUSE is configured; the prescribed
privileged Linux container runs them for real. Portable tests cover action-index persistence,
rebuild, the read-only publication default, mixed-cost size-cap eviction, SLSA provenance derivation
and verification, tamper rejection, DSSE/in-toto shape plus digest round-trip, shared-model
SPDX/CycloneDX structure, fully marked system-binary inference, and cursor pages over public
results, attestations, traces, and result-scoped SBOMs.
The SBOM finalization bound publishes 24 producer actions and gives one final action 96 reads and
24 outputs; result creation and cache validation must complete within twenty seconds.
`test_content.cpp` covers canonical output-tree paths and modes plus read-only named dependency
folding. `test_normalise.cpp` proves two `ar` headers with different owner, group, mode, and time
normalise identically, timestamp-form Python bytecode becomes one payload-addressed form, fixed-root
binary and text kinds are unchanged, and tree modes preserve only portable executability. Its
output-collection cases (liquid #425) prove that a symbolic link to a host file, a linked declared
output or parent directory at any depth, and a FIFO fail collection without reading or rewriting
the target or staging anything; that escaping declared paths are refused; and that regular outputs
are staged and normalised while the action's own copy stays untouched. On Linux,
`Build.output_symlink_fails_before_its_host_target_is_read_or_rewritten` runs an action that links
`out/payload.pyc` to a host sentinel and requires the action to fail with the sentinel unchanged.
Like the other sandboxed action tests it needs FUSE, and skips with the probe's reason without it
(liquid#549).
The provenance tests also enumerate the stored signed input/output claim.
`test_conflicts.cpp` proves that reaching a conflict entry fails materialisation with its path and
digest while a tree containing only the unaffected input materialises normally.

`test_relocation.cpp` checks clean relative metadata and scripts, embedded virtual-root offsets,
absolute pkg-config and CMake paths, and an absolute Mach-O install name. The `wrap/relocation`
fixture installs a shared library, executable, header, pkg-config file, and CMake package export.
The mount-backed wrapper case materialises the tree at one path, moves it, executes it, rejects an
embedded prefix, and verifies that an allowed hit is retained in `why` diagnostics.

`test_abi.cpp` decodes digest-pinned ELF, Mach-O, and PE fixtures; extracts native shared-library
exports; checks compatible additions, removals, changed signatures, object sizes, GNU symbol
versions, and debug signatures; round-trips stored descriptors; verifies the signed compatibility
attestation; and pins JSON ordering. The privileged `wrap/abi` fixture rebuilds a C shared library
with a removed function and changed signature, then requires the `abi-break:` event and
`--fail-on-abi-break` status. The repository-backed case also invokes a result-qualified CLI diff
between its two stored builds.

Filesystem fixtures allocate their named directories beneath the unique process root specified in
the parent [`README.md`](../README.md).

`test_events.cpp` covers subscription-point fan-out, terminal failure ordering, frontend-shaped
diagnostics, and exact JSON-lines schema stability for the public build stream and configured
plan. Public plan cases cover the named unconfigured error, root digest, complete inputs,
output-derived dependency edges, one-call repository execution, and daemon round trips. The Linux wrap
scenario edits a second input while a delayed action runs and checks that re-planning reports the
untouched actions as hits. The mount-backed own-tree measurement edits a function body in
`src/store/faults.cpp` inside a repository snapshot and prints call-to-start latency, event count,
and load average.

`test_graph.cpp` covers canonical demand, read-order dependencies, red-green validation, early
cutoff, durability-filtered invalidation counts, cancellation and discarded computations. Clean
validation cases cover the default one-in-twenty selection, a zero-divergence full pass, and a
deliberately stale value that is named with its input, audited, evicted, recomputed, and retained
as a divergence record after reopen. Its
restart case kills `graph_crash_fixture.cpp` during an active demand, reopens the last complete
root, counts one input fingerprint read and no query executions, and checks the public recorded
query hook.

`test_frontend.cpp` loads the Clang frontend dynamically, registers its descriptor statically,
checks that its shared object exports only the ABI v1 entry point, and asks the tree-sitter fallback
for a C definition. It also checks the optional parse tail and proves that a build followed by a
structural diff parses each distinct blob once. Its copied CMake fixture uses `VALUE` from a quoted header and compiles two
definition objects with the configured compiler. A one-body edit executes one action and assembly,
while a changed quoted header executes both recorded readers and assembly.
`frontend-one-function-measure` is a registered macOS/Linux
CTest that performs the same count assertion against `src/store/faults.cpp` and prints its elapsed
milliseconds.
`test_grammars.cpp` parses the lock-driven sample for every configured grammar without an `ERROR`
node, runs tag queries for Python, Go, Rust, JavaScript, TypeScript, Java, and Ruby, checks explicit
unsupported behavior when no tag query exists, rejects a descriptor with a mismatched grammar ABI,
and checks public registry metadata. `liquid-grammar-measure` reports mean nanoseconds over 100
parses of each fixture.
`test_structural_diff.cpp` exercises handwritten move, formatting, and semantic edits for the ten
primary languages, mechanically reindents every other locked sample, and checks comment handling
and public repository tiers.

`test_edit.cpp` covers concrete syntax, compiler-record rename across two translation units,
collision refusal, extract, move, node fingerprint refusal, exact after digests, whole-operation
undo, daemon framing, and CLI JSON. It compiles edited sources with the configured compiler,
including a copied `src/store/faults.cpp` and a two-file shared-header fixture.
`liquid-edit-measure` indexes liquid's own source with tree-sitter and reports references, indexed
files, touched files, text changes, elapsed milliseconds, and the separately sampled host load.

`test_distribution.cpp` measures tiny, short, long, and uncertain timing histories against the
cost model and asserts local, remote, and race choices with exact estimates. Its second-store test
executor uses #17's shared on-disk remote source to reconstruct the source and toolchain in a new
directory, matches a local output, groups four short actions through one batch call, and proves a
race cancels its delayed remote loser while adding one published result.

`test_why.cpp` builds signed action/result histories without a mount and proves input digests and
line ranges, drip attribution, argv, environment, toolchain, sandbox, never-built, imported-index,
execution scheduling inputs, and cumulative-statistics explanations. The Linux `test_wrap.cpp`
scenario additionally proves that
a real observed header miss is available through `build::why` and that completed-wrap action and
byte counts agree. It also tightens policy after a recorded build and proves `why` reports the
current refusal before treating the previous lookup as a hit or ordinary miss.

`fixture.c` is the hermetic toolchain program materialised into each test repository. Its commands
copy or read one input, print the fixed environment, emit random bytes, attempt denied access, or exercise
timeout cleanup. Its deterministic failure command writes fixed output, stdout, and stderr before
returning a nonzero exit for replay coverage. Verification tests prove a pure agreement,
signature-valid agreement and
disagreement attestations, rate-one sampling, durable divergent quarantine, and critical output
diagnostics.
Its shell-compatible `-c` test mode creates a small prefix install tree for the Linux Spack import
subset, allowing imported target execution and relocation publication to use the ordinary action
path without host tools.

Replay cases prove byte-identical reproduction of that recorded failure, action selection after a
cache hit, opt-in verification recording, byte-zero diagnostics for an altered recorded output,
and hydration of a missing input through a `remote_blob_source` backed by a second on-disk liquid
store. The Linux CLI case also requires valid JSON and confirms default replay does not record.

The mixed-cost retention case writes a cheap large result and an expensive small result into the
real store, persists both index rows, exceeds the configured physical cap, and proves collection
removes only the lower execution-cost-per-byte result.

`wrap/` holds unchanged CMake, Autotools, recursive Make, compilation-database, install-tree, and
shared-library
projects built
with the container's system toolchain. `test_wrap.cpp` proves inner and outer hits, selective header
invalidation with an explanation, fixed-root reuse in a second checkout, cached configure probes,
cross-project configure facts with zero feature probes on reuse, foreign-target refusal before
compiler entry, parallel compilation-database seeding, and sandbox-only hook variables. The Linux
CLI case runs `liquid verify --all` and `liquid verify --graph --all`, checks that every wrapped
action and query agrees, then replays one action through `liquid replay --json`.
It also parses every line from a failing `liquid build --events` run and checks that the command
returns the recorded status.
The configured-build CLI case parses every `liquid build --plan` line and checks the recorded
compile and link actions, complete input fields, and dependency arrays.
The CMake fixture also compares its generated SBOM components with every target manifest digest
and toolchain-tree leaf. The shared-library fixture exercises automatic ABI publication and the
compatibility gate. The query-graph cases make a comment-only translation-unit edit execute
one compiler while preserving the object and link result, then make a semantic edit execute the
compiler and linker and change the link result.
The fixture registers a labelled smoke test, and the wrap scenario verifies its public target
metadata.
Its cross-project case compiles an upstream object twice across a comment-only source edit, imports
both signed result claims into a downstream project, and asserts that the identical output tree
causes zero downstream processes and a named unchanged-input explanation.

`content-addressed-chain.cmake` is the opt-in `realworld` fixture. It fetches zlib 1.3.1 and libpng
1.6.44 through pinned SHA-256 archives, builds zlib, builds libpng from zlib's named output tree,
and links a small program from libpng's named output tree. It asserts the zero-process downstream
comment-only rebuild and the changed-object link path, then prints all process counts and host load
average.

`test_facts.cpp` is portable coverage for canonical probe normalization, durable records and
bundles, and exact isolation across changed toolchain, sysroot, and target keys.

`test_symbols.cpp` verifies the Clang frontend's derived action description, compiler-output
record decoding, stable cross-file reference identifier, repository-store round trip, and the
explicitly labelled tree-sitter fallback. The Linux `symbols-cli` fixture records a CMake-fixture
compile with `liquid wrap`, then verifies compiler symbols for its header; macOS excludes that
fixture because wrapped action execution requires macFUSE.
The portable `symbols-cli-python` case verifies extension selection and a syntactic Python result.

The real identical-execution cache test verifies every result's provenance and asserts that a hit
reuses the original attestation digest.

`sleep_copy.c` is a dynamic test tool used only by the privileged Linux wrap scenario. Two
invocations with different sleeps form a known output dependency. The scenario verifies the
longest dependent chain, exact phase-sum accounting, retained trace loading, Chrome JSON structure,
recorded scheduling fields, and `why_slow` target selection.
`test_policy.cpp` covers fail-closed refusal, warning reports, approved-builder filtering,
invalid-signature and unknown-profile rejection, two-distinct-identity reproducibility quorum,
toolchain traversal, subject-index rebuild after reopen, pre-spawn refusal in the Linux mount
environment, and policy-bound candidate keys.

`test_remote_outputs.cpp` uses a second on-disk liquid store as a counting remote source. It proves
zero-byte unused results, exact chunk-range hydration and cache reuse, on-demand materialisation,
tri-state availability, and metadata-only consumption by a consecutive remote result. The Linux
executor fixture marks one action eligible and verifies `/etc/hostname` is refused and reported.

`test_wrap_plan.cpp` covers what the wrap hook trusts (liquid#401): two inputs with the same
FNV-1a value no longer satisfy each other's conditional row, a condition in the old FNV-1a form is
a miss, a plan is read only when its bytes hash to the digest liquid passed, and a shared or linked
plan is refused. For the process key (liquid#449) it checks that two command lines whose old
FNV-1a keys collide get different BLAKE3-256 keys and that a row for one is not a hit for the other,
that each part of a key is hashed with its length, that a row with a key in any other form never
hits, and that a saved wrap state without the version-2 line is ignored. On macOS it preloads the real hook into `wrap_probe.c` and checks that the tool
runs unless a bound, private plan beside the mount matches.
