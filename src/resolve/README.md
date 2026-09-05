# Resolution implementation

`spec.cpp` implements the pure public spec parser and formatter. `records.cpp` owns canonical
`package/1` and `concretised_spec/1` codecs plus package-universe loading. `concretizer.hpp` is the
private boundary between the package encoding and the in-tree CDCL engine.

All solver CPU phases run through `runtime::offload`; repository reads remain coroutine-native.
