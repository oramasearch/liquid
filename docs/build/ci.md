# Continuous integration

GitHub Actions runs [`.github/workflows/ci.yml`](../../.github/workflows/ci.yml) for every push
and pull request. Checkout recursively initializes the public Capy plus the private Corosio and
automerge-cpp submodules using `ORAMA_SUBMODULES_TOKEN`. The repository secret must exist and
grant read access to `oramasearch/corosio` and `oramasearch/automerge-cpp`; checkout fails before
the build when it is absent. Local clones should use `git clone --recurse-submodules`. Each job
then builds without a dependency cache, runs CTest with failure output, and runs `liquid version`
on each desktop platform.

| Job | Runner | Compiler and build tools | Verification |
|---|---|---|---|
| Linux | `ubuntu-24.04` | Clang 19 from apt.llvm.org, `libstdc++-14-dev`, CMake 3.28 or newer, Ninja | Configure, build, CTest, `liquid version` |
| macOS | `macos-15` | Homebrew LLVM, CMake 3.28 or newer, Ninja | Configure, build, CTest, `liquid version` |
| Windows | `windows-2022` | The runner's current `clang-cl` with the MSVC STL, current Chocolatey CMake and Ninja | Configure, build, CTest, `liquid version` |
| Linux sanitizers | `ubuntu-24.04` | Clang 19 with AddressSanitizer and UndefinedBehaviorSanitizer | Debug configure, build, CTest |

`CMakeLists.txt` selects Homebrew LLVM on macOS, a versioned `clang++` on Linux, and `clang-cl`
for both C and C++ on Windows before `project()`. GoogleTest enables C when FetchContent adds it;
using `clang-cl` for that language prevents CMake from selecting GNU-style `clang.exe`, which
cannot consume the Visual Studio flags passed by its developer environment. If no compiler is
found, configuration stops with instructions for installing one or choosing an explicit system
compiler. The Windows job updates CMake before configuration because the image's CMake 3.31 does
not recognize `cxx_std_26` for Clang 19.1.5; the current package maps that requirement to
`clang-cl`'s `/std:c++latest`. The workflow relies on this paired automatic selection rather than
overriding only the C++ compiler. It initializes the Visual Studio developer environment so
`clang-cl` uses the MSVC STL and linker.

## Platform coverage

`macos-15` is an Apple Silicon runner, so the matrix includes arm64 coverage through macOS. Linux
is currently x86_64 only: GitHub-hosted arm64 Linux runners are not available to this private
repository at no cost. Linux arm64 verification is deferred until a suitable runner is available.

The sanitizer job is separate from the release-style matrix. It configures a Debug build with
`-fsanitize=address,undefined` and runs CTest; a sanitizer finding fails the workflow.
