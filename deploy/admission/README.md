# Admission container

This image packages the Linux `liquid` executable with `serve admission` as its entry point. The
default build context expects `build-linux/liquid`; `LIQUID_BINARY` may select another Linux
artifact. The runtime uses Ubuntu 24.04 with runtime libraries only and runs as `65532:65532`;
`LIQUID_BASE` is no longer supported. See the [image contract](../../docs/build/service-images.md).

Mount the versioned policy at `/etc/liquid/admission.json`, the k0s-issued serving certificate and
key at `/etc/liquid/tls/tls.crt` and `/etc/liquid/tls/tls.key`, and a local attestation store at
`/var/lib/liquid-admission`. State and `/var/lib/liquid-identity` must be writable by UID 65532;
mounted policy and TLS files must be readable by it. Port 8443 is the webhook and 8083 is the
loopback-only control surface. A deployment
using the node-store source should replace `--store` with `--attestation-source <url>`.

The Kubernetes `ValidatingWebhookConfiguration`, CRD, certificate issuance, and deployment remain
owned by oramasearch/k0s#15. See [`../../docs/library/admission.md`](../../docs/library/admission.md)
for policy, readiness, and fail-closed behavior.
