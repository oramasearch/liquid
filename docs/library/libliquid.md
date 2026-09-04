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
the version API; `errc`, `error` and `result<T>`; algorithm-tagged digests with BLAKE3 and
SHA-256; plain object, drip and identity values; and opaque store, repository, capture, signer,
key-provider, mount and remote handles. Hashing, digest text conversion and the error model are
implemented. Operations owned by later issues return `errc::unsupported` and name that issue in
`error::context`. Rules: `std::expected` at the boundary, no exceptions across it, **never a
Zetta type**, and no third-party type in a public header. The CLI never carries behaviour the
library lacks.

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
`nm -gU build-shared/libliquid.dylib | grep liquid` lists exactly the exported API. For an
install consumer, install to a temporary prefix, configure a separate CMake project whose
`CMakeLists.txt` calls `find_package(liquid CONFIG REQUIRED)` and links its executable to
`liquid::liquid`, then build and run that executable with the prefix in `CMAKE_PREFIX_PATH`.

**Change.** New public headers go under `include/liquid/` and into `liquid.hpp`; new sources into
the `libliquid` target. The public surface, its rules and the ABI policy are
[`api-map.md`](api-map.md) (liquid issue #39): a header that is not on that map is not public.
