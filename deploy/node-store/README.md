# Node store container

The image packages `liquid node-store serve` beside the worker images. Its default
Ubuntu 24.04 runtime includes libfuse3 and its mount helper, without compilers, and runs as
`65532:65532`. The control listener defaults to loopback. See the
[image contract](../../docs/build/service-images.md); `LIQUID_BASE` is no longer supported.

The process needs `/dev/fuse` plus the mount capability in the containerd snapshotter's mount
namespace. A simple container run therefore uses `--device /dev/fuse --cap-add SYS_ADMIN`; the k0s
integration may instead place the process directly in the snapshotter's mount namespace. Mount
the node socket directory so the proxy snapshotter can reach `/run/liquid/node-store.sock`, and
persist `/var/lib/liquid-node-store` across process restarts.
Mounted state, `/var/lib/liquid-identity`, and the socket directory must be writable by UID 65532.
The image creates the state and identity directories with mode 0700, and `/run/liquid` with mode
0755. Existing volumes need their ownership updated before switching to this image; a mount hides
the directory ownership shipped in the image. FUSE device access and the required mount namespace
remain deployment responsibilities; the image does not start as root to obtain them.
With default FUSE owner access, processes consuming the mounted image paths must also use UID
65532 in a compatible user namespace. Coordinate this change with the proxy snapshotter; see the
[FUSE access notes](../../docs/build/service-images.md#state-and-permissions).

The containerd proxy snapshotter and drop-in are k0s #14 work. This image contains neither a
snapshotter nor writable layers.
