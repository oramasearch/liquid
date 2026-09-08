# Installed CMake consumer

This is a separate CMake project that consumes the installed liquid package. Its executable finds
the package with `find_package(liquid CONFIG REQUIRED)` and links only `liquid::liquid`.
It creates a repository, writes and reads a file, closes the handle, opens `"."` through
`runtime::block_on(repository::open("."))`, and verifies the persisted bytes. Its own Capy
coroutines await repository operations and explicitly yield through the executor. Errors and
content mismatches fail the process even in Release builds.

The parent suite registers `consumer-project-build`. That CTest step installs the already-built
parent artifacts into a test-local prefix, creates a clean consumer build with the parent's
generator and C++ compiler after moving the prefix, rejects source/build paths in installed CMake
metadata, and builds the library consumer and an installed `liquid_forge_double`
header/target consumer, and runs them. A separate `liquid-hot-consumer` retains the identity decoder
and separately versioned hot-update ABI checks. The repository fixture lives in the clean consumer
build directory. It verifies the static or shared installed package and forge test target without
rebuilding liquid or its dependencies; each build uses at most six jobs.
