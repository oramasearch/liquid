# Resolution implementation

`spec.cpp` implements the pure public spec parser and formatter. `records.cpp` owns canonical
`package/1` and `concretised_spec/1` codecs plus package-universe loading. `concretizer.hpp` is the
private boundary between clause grounding and the in-tree CDCL engine in `concretizer.cpp`.
`resolve.cpp` offloads both CPU phases and projects stable graphs, conflict JSON, and `why`
explanations.

The engine learns a blocking clause at conflicts, backtracks, enumerates viable models for the
lexicographic objective, and deletion-minimises an attributed core when no model exists. All solver
CPU phases run through `runtime::offload`; repository reads remain coroutine-native.
