# OSTree writer

This directory owns liquid's dependency-free OSTree object and repository writer. `ostree.cpp`
encodes only the fixed GVariant schemas used by file headers, dirtrees, directory metadata,
commits, and summaries. It calculates every SHA-256 checksum from those canonical bytes, retains
regular-file and symbolic-link modes, and takes the commit timestamp from `SOURCE_DATE_EPOCH`.

The repository output modes are `archive-z2`, using deterministic raw DEFLATE `.filez` objects,
and `bare-user`, using direct content plus `user.ostreemeta`. Bootc projection consumes the same
in-memory object set to build its `bare-split-xattrs` OCI layers. CPU and filesystem work is called
through `runtime::offload` by the projection layer.

The format contract, GVariant signatures, bootc labels, and interoperability proof are documented
in `docs/library/projection.md`.
