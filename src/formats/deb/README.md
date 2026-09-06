# Debian packages

The writer emits a deterministic `ar` archive containing `debian-binary`, `control.tar.zst`, and
`data.tar.zst`. It owns every archive header and uses the shared deterministic tar and pinned-zstd
writers. The reader validates the archive and exposes control text and installed paths for the
round-trip suite. Maintainer scripts are intentionally absent until issue #80.
