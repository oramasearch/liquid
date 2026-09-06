# Source implementation

`main.cpp` owns the public CLI command table and dispatches each command to its focused adapter
under `cli/`. Library implementation directories follow the installed public API areas while
keeping parsers, codecs, storage, network, solver, and runtime implementation types private.

Spack package-universe import lives under `import/`; it is dispatched by the existing import
command group and exposed only through `include/liquid/import.hpp`.
