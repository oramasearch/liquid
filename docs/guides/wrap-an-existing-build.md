# Wrap an existing build

`liquid wrap` runs an unchanged build in an observed working tree. The first run records each
output-producing tool invocation and its actual reads. A later run validates those reads and can
restore a result before the tool starts. The cache therefore includes command arguments,
environment, toolchain, sandbox definition, and every observed input rather than only declared
dependencies.

## CMake

This executable example uses the repository's CMake fixture. In another project, start with its
ordinary configure and build commands. The test runner provides `LIQUID_GUIDE_FIXTURES`; it is not
required for a normal project.

<!-- guides-run: wrap-cmake -->
```sh
cp -R "$LIQUID_GUIDE_FIXTURES/cmake" cmake-project
cd cmake-project
liquid wrap cmake -S . -B build -G Ninja
liquid wrap cmake --build build
cmake --build build --target clean
second_run=$(liquid wrap cmake --build build)
printf '%s\n' "$second_run"
printf '%s\n' "$second_run" | grep 'misses=0'
printf '#define VALUE 9\n' > include/value.h
miss_run=$(liquid wrap --explain cmake --build build)
printf '%s\n' "$miss_run"
liquid why build/CMakeFiles/wrapped.dir/left.c.o
liquid provenance build/wrapped > provenance.json
grep 'buildDefinition' provenance.json
```

The clean removes materialised outputs but leaves the build graph. The second wrapped build must
report zero misses: its compiler and linker results come from the local cache. Changing the header
then invalidates only actions that read it. `liquid why` names that observed input, and provenance
is the verified, result-bound record of the command, toolchain, inputs, outputs, and builder.

## Autotools

Autotools configuration and feature probes use the same mechanism. Configure facts are additionally
content-keyed by compiler, explicit sysroot, and target triple so another project can reuse an
equivalent answer without starting the probe.

<!-- guides-run: wrap-autotools -->
```sh
cp -R "$LIQUID_GUIDE_FIXTURES/autotools" autotools-project
cd autotools-project
autoreconf --install
liquid wrap ./configure
liquid wrap make
rm -f wrapped
second_run=$(liquid wrap make)
printf '%s\n' "$second_run"
printf '%s\n' "$second_run" | grep 'misses=0'
./wrapped
```

`LIQUID_WRAP_CACHE` relocates the project cache. Cache publication remains off unless explicitly
enabled by repository policy or a remote option; a cache hit is still signature-, manifest-, and
referenced-object-validated before any output is served.

Windows currently reports that wrapping is unsupported. Linux requires FUSE. macOS requires
macFUSE, and its sandbox guarantee is weaker than Linux namespaces.
