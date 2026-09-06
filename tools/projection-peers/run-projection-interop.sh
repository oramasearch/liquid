#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <build-directory>" >&2
  exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: Docker is unavailable; projection interoperability was not run"
  exit 0
fi
if ! command -v helm >/dev/null 2>&1; then
  echo "SKIP: Helm is unavailable; projection interoperability was not run"
  exit 0
fi

repo_root=$PWD
build_dir=$1
tests="$repo_root/$build_dir/tests/liquid-tests"
temporary=$(mktemp -d)
artifacts="$temporary/artifacts"
k0s_name="liquid-projection-k0s-$$"

debian_image="debian:bookworm@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443"
fedora_image="fedora:42@sha256:99e203b80b1c3d8f7e161ec10a68fd02b081ef83a3963553e513c82846b97814"
alpine_image="alpine:3.21@sha256:48b0309ca019d89d40f670aa1bc06e426dc0931948452e8491e3d65087abc07d"
ubuntu_image="ubuntu:24.04@sha256:33ceb71981b602c1a7443a53469e4dba065f7503eab3078a2d7a57a2ab987517"
arch_image="archlinux:base@sha256:1eb3314f8f66ab90c8c755acaf9a5502c705b81104cb885ea69f1c74f3a6bb80"
k0s_image="k0sproject/k0s:v1.31.1-k0s.0@sha256:c368ea0ab665806e37302c7b26a86f50072a59299310a7f023a534686aab0e3a"

cleanup() {
  docker rm -f "$k0s_name" >/dev/null 2>&1 || true
  rm -rf "$temporary"
}
trap cleanup EXIT

LIQUID_PROJECTION_PEER_OUT="$artifacts" \
  "$tests" --gtest_filter=Projection.emit_peer_artifacts_when_requested

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$debian_image" \
  sh -c 'dpkg -i /artifacts/deb/*.deb && /usr/bin/liquid-fixture'
echo "Debian dpkg install and executable: PASS"

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$fedora_image" \
  sh -c 'rpm -i /artifacts/rpm/*.rpm && /usr/bin/liquid-fixture'
echo "Fedora rpm install and executable: PASS"

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$alpine_image" \
  sh -c 'apk add --allow-untrusted /artifacts/apk/*.apk && /usr/bin/liquid-fixture'
echo "Alpine apk install and executable: PASS"

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$arch_image" \
  sh -c '
    set -e
    pacman -Syu --noconfirm base-devel
    cp -a /artifacts/pkgbuild /build
    useradd -m builder
    chown -R builder:builder /build
    su builder -c "cd /build && makepkg --noconfirm"
    pacman -U --noconfirm /build/*.pkg.tar.zst
    /usr/bin/liquid-fixture
  '
echo "Arch makepkg, pacman install, and executable: PASS"

appimage=/artifacts/appimage/liquid-fixture-1.2.3-x86_64.AppImage
docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$ubuntu_image" \
  "$appimage" --appimage-extract-and-run
echo "Ubuntu AppImage extraction and executable: PASS"

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$fedora_image" \
  "$appimage" --appimage-extract-and-run
echo "Fedora AppImage extraction and executable: PASS"

if [[ -e /dev/fuse ]]; then
  docker run --rm --platform linux/amd64 --device /dev/fuse --cap-add SYS_ADMIN \
    --security-opt apparmor:unconfined -v "$artifacts:/artifacts:ro" "$ubuntu_image" \
    "$appimage"
  echo "Ubuntu AppImage FUSE mount and executable: PASS"
else
  echo "SKIP: /dev/fuse is unavailable; AppImage FUSE mount was not run"
fi

docker run --rm --platform linux/amd64 -v "$artifacts:/artifacts:ro" "$ubuntu_image" \
  sh -c '
    set -e
    apt-get update
    apt-get install --yes snapd squashfs-tools
    snap pack /artifacts/snap/liquid-fixture-1.2.3.snapcraft/prime /tmp/liquid-fixture.snap
    unsquashfs -l /artifacts/snap/liquid-fixture_1.2.3_amd64.snap |
      grep -F squashfs-root/usr/bin/liquid-fixture
    unsquashfs -l /tmp/liquid-fixture.snap |
      grep -F squashfs-root/usr/bin/liquid-fixture
  '
echo "Snap pack and SquashFS inventories: PASS"

image_layout="$artifacts/oci/liquid-fixture-1.2.3.oci"
tar -C "$image_layout" -cf "$artifacts/liquid-fixture.oci.tar" .
helm lint "$artifacts/helm/liquid-fixture"
echo "Helm lint: PASS"

docker run -d --rm --privileged --platform linux/amd64 --name "$k0s_name" \
  -p 6443:6443 -v "$artifacts:/artifacts:ro" "$k0s_image" \
  k0s controller --single --enable-worker --no-taints >/dev/null
for _ in $(seq 1 60); do
  if docker exec "$k0s_name" k0s kubectl get nodes >/dev/null 2>&1; then
    break
  fi
  sleep 1
done
docker exec "$k0s_name" k0s kubectl get nodes
docker exec "$k0s_name" k0s ctr images import /artifacts/liquid-fixture.oci.tar
docker exec "$k0s_name" k0s kubeconfig admin >"$temporary/kubeconfig"
sed -i.bak 's#server: https://localhost:6443#server: https://127.0.0.1:6443#' \
  "$temporary/kubeconfig"
helm install liquid-fixture "$artifacts/helm/liquid-fixture" \
  --kubeconfig "$temporary/kubeconfig" --wait
docker exec "$k0s_name" k0s kubectl wait --for=condition=Ready pod \
  -l app.kubernetes.io/name=fixture
echo "k0s OCI import, Helm install, and pod Ready: PASS"
