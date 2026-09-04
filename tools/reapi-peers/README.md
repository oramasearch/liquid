# REAPI interoperability peers

`nativelink.json5` starts a local NativeLink CAS, capabilities service, and worker endpoint on
port 50051. It is intentionally plaintext so HTTP/2 prior-knowledge h2c is the phase proof.

From the Linux CI container, run the automated bidirectional probe after building:

```sh
tools/reapi-peers/run-h2c-interop.sh build-linux
```

It starts NativeLink with host networking, runs liquid's client `GetCapabilities` exchange, then
starts a one-request private HTTP/2 test server and uses `grpcurl -plaintext` to send an Echo RPC
to it. The script prints a skip diagnostic and exits successfully when Docker is unavailable.

To run the client direction against an already-running peer:

```sh
LIQUID_HTTP2_NATIVE_LINK=1 build/tests/liquid-tests \
  --gtest_filter=Http2Interop.GetsCapabilitiesFromNativeLinkOverH2c
```

`liquid_echo.proto` is only the grpcurl wire-format fixture; it is not part of liquid's API. TLS
peer coverage waits for Corosio #3 to make ALPN selection observable.
