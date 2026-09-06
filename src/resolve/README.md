# Resolution implementation

`spec.cpp` implements the pure public spec parser and formatter. `records.cpp` owns canonical
`package/1` and `concretised_spec/1` codecs plus package-universe loading. `concretizer.hpp` is the
private boundary between clause grounding and the in-tree CDCL engine in `concretizer.cpp`.
`resolve.cpp` offloads both CPU phases and projects stable graphs, conflict JSON, and `why`
explanations.

The engine learns a blocking clause at conflicts, backtracks, enumerates viable models for the
lexicographic objective, and deletion-minimises an attributed core when no model exists. All solver
CPU phases run through `runtime::offload`; repository reads remain coroutine-native.

`graph_builder` constructs the selected graph in postorder with an explicit frame stack rather than
native recursion: `open_frame` marks a package in progress and plans its dependency and compiler
edges, and `close_frame` binds the edge digests, hashes the canonical record, and publishes the
node once every edge target exists. At most one frame is open per selected package, so a long
dependency chain costs heap frames, never worker-thread stack. That stack is 512 KiB on a macOS
pool thread, and one native frame per link overflowed it at 500 packages in Release builds
(issue #263).
