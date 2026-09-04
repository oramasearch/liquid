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
`error::context`. Waiting operations return lazy `runtime::task<T>` values and are exercised from
synchronous code through `runtime::block_on`. Rules: `std::expected` at the boundary, no
exceptions across it, **never a Zetta type**, and no third-party type in a public header except
Boost.Capy through `liquid/runtime.hpp`. The CLI never carries behaviour the library lacks.

**Consume.**

Clone with both vendored runtime dependencies:

```sh
git clone --recurse-submodules git@github.com:oramasearch/liquid.git
```

```cmake
# as a submodule (liquify)
set(LIQUID_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/liquid EXCLUDE_FROM_ALL)
target_link_libraries(app PRIVATE liquid::liquid)

# from an install
find_package(liquid CONFIG REQUIRED)
target_link_libraries(app PRIVATE liquid::liquid)
```

When a parent project already defines `Boost::capy` or `Boost::corosio`, liquid reuses that target
instead of adding its vendored checkout again. The supported minimum revisions are the revisions
used by liquid's own checkout; a parent-supplied target must come from that commit or a compatible
descendant on the named branch.

| Dependency | Tested revision | Target and rule |
|---|---|---|
| Capy `develop` | `ce1c7d710c4baaeeddb57fa101707c610f6eb975` | `Boost::capy`; liquid adds `modules/capy` only when the target is absent |
| Corosio `orama-main` | `9f8e865a7b5728a4093e9345456bc077194dbeff` | `Boost::corosio`; liquid adds `modules/corosio` only when the target is absent |
| nlohmann/json | bundled `8167d2f64122c3ef7e5a0d9cab239fdba37aa54c`; supplied minimum 3.11.3 at `9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03` | `nlohmann_json::nlohmann_json`; automerge-cpp uses a parent target when present, and the lake-shaped consumer proof supplies it before liquid |

`tests/consumer/` is a separate CMake project that supplies all three targets before
`add_subdirectory(liquid)`, builds a program linked to `liquid::liquid`, and runs it. The main CTest
suite configures and builds that project with the same compiler and static/shared setting as the
parent build.

`cmake --install build --prefix <p>` lays out `bin/liquid`, `lib/libliquid.a`, `include/liquid/`,
`lib/cmake/liquid/` (`liquidConfig.cmake`, `liquidConfigVersion.cmake`, and
`liquidTargets.cmake`), the wrap hook and Linux remote-action read guard under `libexec/liquid/`,
plus Capy's public headers under `include/boost/`. The liquid export owns
Capy's `liquid::capy` library target and recreates its `Boost::capy` alias. It carries its
internal Corosio archive to satisfy static linking, while `libliquid` links Corosio privately.
Consumers receive Capy's headers, compile options, and link library transitively without
installing Capy's test support archives. The package also carries the private Boost.HTTP and Boost
component targets required by the sync client; its config resolves the OpenSSL, zlib, and installed
zstd targets used by static consumers.

**Verify.** Static: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
Shared: `cmake -S . -B build-shared -G Ninja -DBUILD_SHARED_LIBS=ON` then
`nm -gU build-shared/libliquid.dylib | grep liquid` lists exactly the exported API. For an
install consumer, install to a temporary prefix, configure a separate CMake project whose
`CMakeLists.txt` calls `find_package(liquid CONFIG REQUIRED)` and links its executable to
`liquid::liquid`, then build and run that executable with the prefix in `CMAKE_PREFIX_PATH`.
Its source should `co_await` a `liquid::runtime::task<T>` so the check also compiles Capy's
exported coroutine headers.

The default CTest suite includes `consumer-project-build`; run it alone with
`ctest --test-dir <build> -R '^consumer-project-build$' --output-on-failure`.

**Change.** New public headers go under `include/liquid/` and into `liquid.hpp`; new sources into
the `libliquid` target. The public surface, its rules and the ABI policy are
[`api-map.md`](api-map.md) (liquid issue #39): a header that is not on that map is not public.
