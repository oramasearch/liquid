# Spack interchange fixtures

The twenty-package export covers sources, variants, conditional and virtual dependencies,
provides, conflicts, static CMake and Autotools arguments, unknown build systems, dynamic values,
custom phases, and a digest-less patch. The 500-package export extends the same roots with a
transitive chain so the suite exercises a complete 500-node solve.

`spack-560.json` was produced by `tools/spack-export/export.py` from Spack revision
`2bfcc69fa870d3c6919be87593f22647981b648a`; the recorded checkout archive digest is
`sha256:68dc36928d212cf0197b958bba4c0e081270534f856a8d3fde0dab6da170e648`.
The import maps 540 packages and reports every rejected value for the remaining records. Its one
digest-less external patch is mirrored as `hip.patch` for offline injection.

All URLs use the reserved `.invalid` domain. Tests inject the exact digest-less patch bytes and do
not make network requests.
