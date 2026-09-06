# Projection interoperability peers

`run-projection-interop.sh <build-directory>` asks the focused projection test to emit the
committed fixture as Debian, RPM, Alpine, OCI, Helm, Homebrew, Arch, AppImage, Flatpak, and Snap
outputs. It then proves:

- `dpkg -i`, `rpm -i`, and `apk add --allow-untrusted`, followed by executing the installed static
  fixture;
- native `helm lint`;
- OCI layout import into k0s, Helm installation, and a `Ready` pod.
- `makepkg`, `pacman -U`, and execution of the Arch package;
- AppImage extraction and execution on Ubuntu 24.04 and Fedora;
- Snap packing plus `unsquashfs` inventory checks.

`run-homebrew-interop.sh <test-binary> <native-binary> <output-directory>` is the macOS proof. It
creates a temporary tap, serves the projected bottle over a loopback HTTP server selected through
`HOMEBREW_BOTTLE_DOMAIN`, installs the formula from that bottle, and runs `brew test`. It removes
the formula, tap, server, and temporary root on exit.

`run-flatpak-interop.sh <test-binary> <output-directory>` is registered with CTest under the
`realworld` label when Flatpak and Flatpak Builder are installed. It checks the export-ready build
directory with `flatpak build-export` and runs `flatpak-builder --build-only` against the pinned
freedesktop 24.08 runtime and SDK. It skips when those large runtime inputs are not installed. The
manifest and export-ready directory remain covered by portable golden tests when that peer is
unavailable. Snap installation is not attempted because container peers do not run the services
required by snapd.

Debian bookworm, Fedora 42, Alpine 3.21, Ubuntu 24.04, Arch, and k0s 1.31.1 image indexes are pinned
by SHA-256 in the script. Update a tag and digest together, rerun all peers, and record the proof
lines in the pull request. The script uses an isolated temporary directory and container name and
removes both on exit. It prints an explicit skip and exits successfully when Docker or Helm is
unavailable.

The package fixture runs and exits with no arguments. Its OCI config adds `serve`, which keeps the
same static executable alive in the pod without adding a base image or runtime dependency.
