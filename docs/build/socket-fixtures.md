# Unix-domain socket fixtures

Unix-domain socket addresses have a fixed byte budget, independent of filesystem path limits:

| Platform | `sockaddr_un.sun_path` capacity | Maximum pathname bytes with a terminating NUL |
|---|---|---|
| macOS | 104 bytes | 103 bytes |
| Linux | 108 bytes | 107 bytes |

Count encoded bytes, not characters. Liquid's pathname sockets reserve one byte for the terminating
NUL. A deep checkout, a relative `TMPDIR`, or a long temporary directory can exhaust this budget
before a socket is bound. Moving a repository or changing a caller's environment is not a fixture
fix.

## Fixture construction and cleanup

Use [`tests/socket_fixture.hpp`](../../tests/socket_fixture.hpp) for a listening socket on macOS
or Linux:

```cpp
auto socket = liquid::test::socket_fixture{};
const auto endpoint = socket.path();
```

The helper creates a private, unique `lq-sock-XXXXXX` directory with `mkdtemp` beneath the platform
temporary root when its complete socket pathname fits. A relative or overlong root falls back to
`/tmp`; the checkout and build directory never contribute to the socket path. Each instance owns
one socket pathname and removes its directory, including any remaining socket, on scope exit.
Keep the instance alive until all socket users have stopped. Ordinary repository and file fixtures
continue to use `temporary_directory.hpp`.

`HotUpdateDemo.TierOnePatchChangesARunningCapyLoop` uses this helper independently of its repository
directory. The cooperative runtime reports an overlong endpoint as
`socket path exceeds the platform limit (N > 104 bytes including NUL): 'path'` on macOS, with the
native capacity substituted on Linux. `N` includes the terminating NUL; the message names the
offending endpoint instead of reporting a generic invalid endpoint.

## Verification

Run from a deep worktree with the normal environment:

```sh
pwd
cmake -S . -B build -G Ninja
cmake --build build -j 6
ctest --test-dir build --output-on-failure -R '^HotUpdateDemo|^SocketFixture'
ctest --test-dir build --output-on-failure
```

The fixture regressions cover root selection, relative and overlong roots, unique directories,
and cleanup of a bound socket. The runtime boundary regression accepts the largest NUL-terminated
pathname that fits and rejects the next byte with the path, required bytes, and native capacity.

Other existing shapes are intentionally separate: the tier-3 demo's `unused.sock` and
`Repository.HotUpdateConfigurationPersistsAcrossOpen` store configuration-only paths without binding
them; `tests/tpm/run_swtpm.sh` places command and control sockets beneath its own temporary state
directory. Daemon tests exercise the production daemon's digest-derived endpoint in the platform
temporary root. These are not changed by issue #305.
