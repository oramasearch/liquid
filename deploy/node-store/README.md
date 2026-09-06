# Node store container

The image packages `liquid node-store serve` beside the worker images. Its default
`liquid-linux-ci` base already ships the libfuse3 runtime; a production node image must also ship
libfuse3, including the node image tracked by k0s #16.

The process needs `/dev/fuse` plus the mount capability in the containerd snapshotter's mount
namespace. A simple container run therefore uses `--device /dev/fuse --cap-add SYS_ADMIN`; the k0s
integration may instead place the process directly in the snapshotter's mount namespace. Mount
the node socket directory so the proxy snapshotter can reach `/run/liquid/node-store.sock`, and
persist `/var/lib/liquid-node-store` across process restarts.

The containerd proxy snapshotter and drop-in are k0s #14 work. This image contains neither a
snapshotter nor writable layers.
