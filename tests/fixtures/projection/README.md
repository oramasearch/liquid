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

`scripts/` contains the exact four lifecycle scripts generated from the shared install-intent
fixture for Debian, RPM, and Alpine. The projection tests compare every byte, including guards and
quoting.

`debian-source/` is the Tier B fixture: a small C static library, public header, pkg-config file,
manual page, and CLI. Its `.liquid` target supplies the argv and canonical outputs from which
`debian/rules` and the install manifest are generated. The zero toolchain digest is replaced by a
stored fixture-tree digest only when the configured-target test executes it; source projection
itself does not dereference a declared toolchain.

`debian-source-golden/` pins the generated policy metadata byte for byte. The projection test
separately pins the unsigned `.dsc` digest.
