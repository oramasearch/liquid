# liquid builds liquid

**What.** liquid's bootstrap remains CMake and Ninja, but the compile and link step also runs under
`liquid wrap`. Configuration stays outside the sandbox because FetchContent resolves BLAKE3,
Monocypher, zstd, and GoogleTest over the network. The wrapped build runs with networking denied.

**Why.** Building liquid itself exercises a large C and C++ action graph, private submodules,
generated headers, static archives, a shared helper, and the final CLI in one repeatable scenario.
It is the forcing function for path-independent actions and byte-identical local cache results.

## Fixed paths

Wrapped CMake builds are configured through the fixed `/liquid/work` source root on Linux. CMake
writes source and binary directory names into Ninja commands, so configuring at an arbitrary
checkout path would make the action command differ between workers and would point inside the
sandbox at a path that does not exist. Local Linux verification bind-mounts the checkout at
`/liquid/work` before configuring; the CI job uses the same layout.

The wrapped tree also sets `LIQUID_SELF_HOST=ON`. A VFS snapshot does not preserve the configured
tree's host timestamps, so Ninja otherwise tries to regenerate even when the CMake inputs are
unchanged. Regeneration is a configure operation and FetchContent needs the network. The first
clean Linux run identified the boundary:

```text
[1/2] Re-running CMake...
[0/8] Performing download step (git clone) for 'blake3_source-populate'
fatal: unable to access 'https://github.com/BLAKE3-team/BLAKE3.git/': Could not resolve host
ninja: error: rebuilding 'build.ninja': subcommand failed
actions=7 hits=0 misses=7 processes_started=6 probes_started=0 outer=miss
```

`LIQUID_SELF_HOST` sets `CMAKE_SUPPRESS_REGENERATION` for that build tree. It defaults to off, so
ordinary developer builds retain CMake's automatic regeneration.

The next clean run reached zstd's shared-library target even though libliquid links only the static
target. Its versioned-library link action failed because the sandbox filesystem does not implement
symbolic-link creation:

```text
Creating library symlink _deps/zstd_source-build/lib/libzstd.so.1
CMake Error: failed to create symbolic link
'_deps/zstd_source-build/lib/libzstd.so.1': Function not implemented
actions=430 hits=0 misses=430 processes_started=429 probes_started=0 outer=miss
```

The dependency configuration now explicitly disables `ZSTD_BUILD_SHARED` after the mainline
integration. zstd already marks
`libzstd_static` as position-independent, so both static and shared libliquid continue to link the
same private codec without building an unused artifact.

The exec-time hook is different from a build input path. Before self-hosting, CMake compiled the
full `$<TARGET_FILE:liquid-wrap-hook>` value into libliquid. The defect was found directly in both
bootstrap artifacts:

```text
build/libliquid.a
/Users/stevesperandeo/dev/oramasearch/codex-work/wt/liquid/issue-59/build/libliquid-wrap-hook.dylib
build/liquid
/Users/stevesperandeo/dev/oramasearch/codex-work/wt/liquid/issue-59/build/libliquid-wrap-hook.dylib
```

libliquid now receives only the hook filename. A build-tree CLI finds the hook beside itself, a
test executable finds it in its parent build directory, and an installed CLI finds it under
`libexec/liquid`. `LIQUID_WRAP_HOOK` remains the explicit override.

## Verification record

The Linux two-run action counts, hit rates, critical path, largest rebuilt outputs, and bootstrap
comparisons are recorded here after the privileged mount-backed proof. macOS builds static and
shared libliquid normally; a wrapped macOS build requires macFUSE.

## Bounded restoration

The Linux proof container has an 8 GiB memory limit. Cache restoration has a serial writer: one
output file is in flight and it reads at most 64 KiB from the content store at a time. Repository recovery uses a
64 KiB journal budget: each journal entry is a fixed-size pointer to a paged checkpoint, and a
checkpoint is published when accumulated signed drip bytes reach that budget. Tree pages are
stored one at a time; the checkpoint encoder does not construct a second in-memory copy of the
repository tree.

The wrapped filesystem also reports directory-create events. They are retained for dependency
analysis but are not output blobs; restoration skips them and streams only regular-file outputs.

`Repository.restores_many_journaled_outputs_with_bounded_peak_rss` writes and restores 96
small outputs (three times the buffer budget), crossing the journal budget before its configured
checkpoint interval, and requires peak RSS below 512 MiB on
both supported desktop platforms. The test keeps the payload small so it measures buffering,
not capacity.

The Linux FUSE-backed CMake fixture also runs the same wrapped build twice. It requires the
second outer cache hit to remain below 512 MiB peak RSS and verifies that the private repository's
pending-drip count did not advance. The platform-neutral tree-import RSS test covers the same
bound where FUSE is unavailable.

Wrapper source synchronization uses `repository::import_tree`, not an individual write for every
source on every run. It streams the source directory, compares each file's blob identity with the
private repository's current tree, and writes one drip only when entries changed. An unchanged
tree writes no drip. Its retained state is `O(depth + changed entries + 64 KiB journal budget)`.
Ignored generated entries are retained from the prior paged tree, so synchronising sources for a
subsequent wrapped build cannot remove the configured CMake cache from the observed filesystem.
The following change cut retains that deferred root page instead of rebuilding it from an unloaded
entry map, preserving those configured inputs through the wrapper's snapshot record.
For a `cmake --build` invocation, paths below its configured build directory are inputs even when a
previous wrapper action listed them as outputs; source synchronisation imports them rather than
letting the prior-output exclusion hide the CMake cache.
The wrapper obtains that directory from the current `--build` argument and from the preceding
CMake configure action's `-B` argument, so a configured tree remains available to its next build.
The Linux self-host container has an 8 GiB memory limit; that bound applies to source import as
well as result restoration and repository checkpoints.

The mount callback retains an action's manifest rather than every filesystem callback. While the
observed build is running, it keeps one `(process, path, content)` entry for each distinct input,
one global `(path, content)` entry for the outer action, and one write path per process. Repeated
opens and reads of a header replace the existing identity entry. This keeps the in-memory
observation proportional to the cacheable manifests, not to the number of FUSE reads.

## v6 memory-limit diagnosis

The bind-mounted v6 Linux run was killed with exit 137 during action 414 of 482. Its cache and
build directories were host bind mounts; the sampler therefore distinguishes filesystem cache
pages from process memory. The largest sampled cgroup use was 5,102,751,744 bytes, of which
4,157,706,240 bytes were anonymous, 727,392,256 bytes file cache, and 208,134,144 bytes kernel
memory. At that sample, the wrapper was 2,352,103,424 bytes RSS and its children raised the
process-tree RSS to 5,773,623,296 bytes. The final sampled wrapper high-water mark was
2,798,409,728 bytes and the process-tree high-water mark was 4,463,710,208 bytes.

The growing wrapper allocation was the raw FUSE observation vector, which retained an entry for
every repeated `open` and `read` callback until result publication. The observation log now
deduplicates those callbacks by process, path, and content identity while preserving each
action's manifest and the outer manifest. The sampler's final records are retained in
`self-host-v6-first-memory.log` for the killed run.

The self-host proof bind-mounts the checkout, `build-wrapped`, and
`.liquid-build/self-host-cache` at `/liquid/work`. The explicit cache location matters: without
`LIQUID_WRAP_CACHE`, the wrapper uses `/root/.cache/liquid/wrap` on Docker's writable layer, whose
file-backed pages are charged to the container memory cgroup. The proof samples cgroup-v2
`memory.current`, `anon`, `file`, `shmem`, and `kernel` fields alongside wrapper-and-child
`VmRSS`/`VmHWM` every ten seconds with `tools/linux-ci/self-host-memory.sh`; its log is retained
beside the two-run numbers.

An outer-result cache hit is checked before liquid reconstructs prior per-action outputs in its
private repository. The outer result already contains every requested output, so reconstructing
the per-action tree first retained a growing repository and delayed the cache-only completion
path. On an outer miss, the same per-action reconstruction occurs after the lookup and continues
to use the bounded streaming writer.

## Open questions

- Cross-worker cache transport belongs to the remote-cache phase; this issue proves that the local
  action identity is independent of the checkout path.
