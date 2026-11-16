# REAPI interoperability peers

`nativelink.json5` starts a local NativeLink CAS, capabilities service, and worker endpoint on
port 50051. It is intentionally plaintext so HTTP/2 prior-knowledge h2c is the phase proof.
`nativelink-tls.json5` starts the same service on port 50053 with the committed self-signed
certificate and private key. They are test fixtures only.

From the Linux CI container, run the automated bidirectional probe after building:

```sh
tools/reapi-peers/run-h2c-interop.sh build-linux
```

It starts NativeLink with host networking, runs liquid's client `GetCapabilities` exchange, then
starts a one-request private HTTP/2 test server and uses `grpcurl -plaintext` to send an Echo RPC
to it. The script prints a skip diagnostic and exits successfully when Docker is unavailable.

The TLS counterpart proves ALPN and the same two directions with the TLS NativeLink configuration
and `grpcurl -insecure`:

```sh
tools/reapi-peers/run-tls-interop.sh build-linux
```

To run the client direction against an already-running peer:

```sh
LIQUID_HTTP2_NATIVE_LINK=1 build/tests/liquid-tests \
  --gtest_filter=Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c
```

`liquid_echo.proto` is only the grpcurl wire-format fixture; it is not part of liquid's API.

## REAPI client, cache, and server proof

`run-reapi-interop.sh <build-directory>` starts pinned NativeLink and BuildBarn containers and runs
the negotiated CAS/ActionCache client round trip against both. On Linux it also runs the Phase 2
Make wrap fixture with its disk index removed before the second run, proving a 100% remote hit rate
against the 100% local baseline, then runs the read-guard refusal case.

The script next starts `liquid serve reapi` on `127.0.0.1:50161` (a plaintext listener is served
only on loopback) and runs Bazel 7.4.1 twice from clean output bases. The
first stock-client run uploads the genrule result and the second must report one remote cache hit.
The fixture uses an empty WORKSPACE with module resolution disabled, keeping this server proof
independent of registry downloads. Docker absence is an explicit skip; any available peer or
stock-client failure is fatal.
