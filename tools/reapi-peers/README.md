# REAPI interoperability peers

`nativelink.json5` starts a local NativeLink CAS, capabilities service, and worker endpoint on
port 50051. It is intentionally plaintext so HTTP/2 prior-knowledge h2c is the phase proof.

From the Linux CI container:

```sh
docker run --rm --network host -v "$PWD/tools/reapi-peers/nativelink.json5:/config:ro" \
  ghcr.io/tracemachina/nativelink:v1.6.6 /config
grpcurl -plaintext 127.0.0.1:50051 list
```

The integration test reports a skip when Docker or `grpcurl` is unavailable. TLS peer coverage
waits for Corosio #3 to make ALPN selection observable.
