# Projection interoperability peers

`run-projection-interop.sh <build-directory>` asks the focused projection test to emit the
committed fixture as Debian, RPM, Alpine, OCI image, and Helm outputs. It then proves:

- `dpkg -i`, `rpm -i`, and `apk add --allow-untrusted`, followed by executing the installed static
  fixture;
- native `helm lint`;
- OCI layout import into k0s, Helm installation, and a `Ready` pod.

Debian bookworm, Fedora 42, Alpine 3.21, and k0s 1.31.1 image indexes are pinned by SHA-256 in the
script. Update a tag and digest together, rerun all peers, and record the four proof lines in the
pull request. The script uses an isolated temporary directory and container name and removes both
on exit. It prints an explicit skip and exits successfully when Docker or Helm is unavailable.

The package fixture runs and exits with no arguments. Its OCI config adds `serve`, which keeps the
same static executable alive in the pod without adding a base image or runtime dependency.
