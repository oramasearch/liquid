# Admission container

This image packages the Linux `liquid` executable with `serve admission` as its entry point. The
default build context expects `build-linux/liquid`; `LIQUID_BINARY` and `LIQUID_BASE` may select the
acceptance artifact and base image used by the repository's other service images.

Mount the versioned policy at `/etc/liquid/admission.json`, the k0s-issued serving certificate and
key at `/etc/liquid/tls/tls.crt` and `/etc/liquid/tls/tls.key`, and a local attestation store at
`/var/lib/liquid-admission`. Port 8443 is the webhook and 8083 is the control surface. A deployment
using the node-store source should replace `--store` with `--attestation-source <url>`.

The Kubernetes `ValidatingWebhookConfiguration`, CRD, certificate issuance, and deployment remain
owned by oramasearch/k0s#15. See [`../../docs/library/admission.md`](../../docs/library/admission.md)
for policy, readiness, and fail-closed behavior.
