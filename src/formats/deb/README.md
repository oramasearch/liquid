# Debian packages

The writer emits a deterministic `ar` archive containing `debian-binary`, `control.tar.zst`, and
`data.tar.zst`. It owns every archive header and uses the shared deterministic tar and pinned-zstd
writers. The reader validates the archive and exposes control text, payload bytes and modes,
installed paths, and optional `preinst`, `postinst`, `prerm`, and `postrm` scripts. The writer
places fixed issue #80 generated scripts in the control archive with executable mode.
