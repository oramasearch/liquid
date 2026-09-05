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

The dependency configuration now disables `ZSTD_BUILD_SHARED`. zstd already marks
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

The Linux proof container has an 8 GiB memory limit. Cache restoration keeps one output file in
flight and reads at most 64 KiB from the content store at a time. Repository recovery uses a
64 KiB journal budget: each journal entry is a fixed-size pointer to a paged checkpoint, and a
checkpoint is published when accumulated signed drip bytes reach that budget. Tree pages are
stored one at a time; the checkpoint encoder does not construct a second in-memory copy of the
repository tree.

`Repository.restores_many_journaled_outputs_with_bounded_peak_rss` writes and restores 1,024
small outputs, crossing the journal budget repeatedly, and requires peak RSS below 512 MiB on
both supported desktop platforms. The test keeps the payload small so it measures buffering,
not capacity.

## Open questions

- Cross-worker cache transport belongs to the remote-cache phase; this issue proves that the local
  action identity is independent of the checkout path.
