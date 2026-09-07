# Projection interoperability peers

`run-debian-source-interop.sh <liquid-tests> <output-directory>` emits the source-package fixture,
checks the unpacked tree with `debuild -S`, builds the unsigned `.dsc` in a configured Debian
bookworm `sbuild` chroot, and runs `lintian -EvIL +pedantic`. Any lintian error or warning fails the
peer. It then installs the produced binary `.deb` and runs `liquid-source-fixture`. The peer must
provide `debuild`, `sbuild`, `lintian`, and `dpkg`; the script does not create or alter the chroot.

`refresh-debian-path-packages.sh <output-tsv>` runs inside the pinned Debian bookworm peer to
refresh the committed `dpkg-query -S` evidence used for derived `Build-Depends`.

`run-projection-interop.sh <build-directory>` asks the focused projection test to emit the
committed fixture as Debian, RPM, Alpine, OCI, Helm, Homebrew, Arch, AppImage, Flatpak, and Snap
outputs. It then proves:
committed fixture as Debian, RPM, Alpine, OCI image, Helm, OSTree, and bootc outputs. Its existing
distribution checks prove:

- `dpkg -i`, `rpm -i`, and `apk add --allow-untrusted`, followed by executing the installed static
  fixture; packages emitted with an install intent also expose their four native lifecycle script
  names, create their declared system user, carry their generated systemd unit, and issue the
  guarded unit-enable operation through an isolated systemctl probe;
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
The portable suite separately runs the generated Debian `postinst` twice and pins all twelve script
files byte for byte. The peer is the distribution-tool proof for user creation, unit installation,
package removal, and native package-manager acceptance.

On the Linux and Fedora peer lanes, the emitted `ostree/` repository and `bootc/` OCI layout add
these native checks:

```text
ostree --repo=<artifacts>/ostree fsck
ostree --repo=<artifacts>/ostree log liquid/os/x86_64
ostree commit --repo=<comparison-repo> --branch=liquid/os/x86_64 \
  --timestamp=@1700000000 --owner-uid=0 --owner-gid=0 --no-xattrs \
  --tree=dir=<materialised-fixture>
ostree container image pull oci:<artifacts>/bootc
bootc container lint
```

The comparison commit must equal
`01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4`; the bootc layout digest is
`e21597809fa09ce8888f97c3391e1f1328a6d4d6cd1bbee8be488bd139900728`. `bootc container lint` runs
against the pulled root filesystem in its Fedora peer environment. These checks require native
Linux OSTree and bootc tooling; absence is reported as a deferred peer proof, never replaced by a
different local encoder.
