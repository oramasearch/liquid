# Installed CMake consumer

This is a separate CMake project that consumes the installed liquid package. Its executable finds
the package with `find_package(liquid CONFIG REQUIRED)`, links `liquid::liquid`, and calls the
public identity decoder.

The parent suite registers `consumer-project-build`. That CTest step installs the already-built
parent artifacts into a test-local prefix, creates a clean consumer build with the parent's
generator and C++ compiler, builds only the consumer executable, and runs it. It verifies the
static or shared installed package without rebuilding liquid or its dependencies.
