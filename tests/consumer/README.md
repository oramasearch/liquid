# Nested CMake consumer

This is a separate CMake project that reproduces lake's dependency order: it adds liquid's pinned
Capy and Corosio checkouts and nlohmann/json 3.11.3 before adding liquid. Its executable links
`liquid::liquid`, calls the public identity decoder, and uses nlohmann/json so all supplied targets
participate in the build.

The parent suite registers `consumer-project-build`. That CTest step creates a clean nested build,
configures this project with the parent's compiler and static/shared setting, builds the executable,
and runs it. It therefore verifies the real `add_subdirectory` path independently of liquid's own
CMake project.
