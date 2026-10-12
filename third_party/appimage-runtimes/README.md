# AppImage type 2 runtimes

This directory vendors the x86-64 and AArch64 runtime assets served by the
[`AppImage/type2-runtime`](https://github.com/AppImage/type2-runtime) `continuous` release when
liquid pinned them on 2026-09-06. CMake copies the files through local `file://` URLs and checks
their SHA-256 digests before embedding them.

Do not replace one architecture independently or update hashes only here. The source of truth for
provenance, hashes, sizes, licence review, and the update procedure is
[`docs/library/projection.md`](../../docs/library/projection.md#vendored-runtime-provenance).
`LICENSE` is the upstream runtime notice at the source-tree state recorded there.
