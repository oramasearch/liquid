# Projection fixture

`root/` is the committed install tree used by the projection byte-identity tests. Its executable is
a static Linux x86-64 fixture built from `fixture.go` with:

```sh
CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -trimpath -ldflags=-buildid= \
  -o root/bin/liquid-fixture fixture.go
```

The built executable and configuration file are committed, so every projection target has the
same input bytes on macOS and Linux. With no arguments the executable prints a proof line and
exits; an argument keeps it alive for the k0s pod proof.
