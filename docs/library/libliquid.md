# libliquid

**What.** liquid is a library first. `liquid::liquid` (`libliquid`) is the product; the `liquid`
command-line binary, liquify and lake are clients of it. Decided 2026-09-02 (`CLAUDE.md`,
*Decisions* 7).

**Targets.**

| Target | What |
|---|---|
| `liquid::liquid` (`libliquid`) | `libliquid.a` by default; `libliquid.so` / `.dylib` (`SOVERSION 0`) with `-DBUILD_SHARED_LIBS=ON`. Symbols hidden by default; `LIQUID_EXPORT` (from the generated `liquid/export.hpp`) marks the API |
| `liquid-cli` → `liquid` | the binary; `-DLIQUID_BUILD_CLI=OFF` for library-only consumers |
| `liquid-tests` | GoogleTest; `LIQUID_BUILD_TESTS` defaults to ON only when liquid is the top-level project |

**Public API.** Everything under `include/liquid/`; `liquid/liquid.hpp` is the umbrella. Today:
`liquid::version`, `current_version()`, `version_string()`. Rules: `std::expected` at the
boundary, no exceptions across it, **never a Zetta type**, third-party types (Boost.Capy,
Corosio) kept behind it where feasible so a shared build stays viable. The CLI never carries
behaviour the library lacks; scaffolding does not go into the public API.

**Consume.**

```cmake
# as a submodule (liquify)
set(LIQUID_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/liquid EXCLUDE_FROM_ALL)
target_link_libraries(app PRIVATE liquid::liquid)

# from an install
find_package(liquid CONFIG REQUIRED)
target_link_libraries(app PRIVATE liquid::liquid)
```

`cmake --install build --prefix <p>` lays out `bin/liquid`, `lib/libliquid.a`, `include/liquid/`,
`lib/cmake/liquid/` (`liquidConfig.cmake`, `liquidConfigVersion.cmake`, `liquidTargets.cmake`).

**Verify.** Static: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
Shared: `cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON` then
`nm -gU build-shared/libliquid.dylib | grep liquid` lists exactly the exported API. Install +
consumer: install to a prefix, then a two-line CMake project with `find_package(liquid CONFIG)`
builds and runs.

**Change.** New public headers go under `include/liquid/` and into `liquid.hpp`; new sources into
the `libliquid` target; the API surface design is liquid issue #39.
