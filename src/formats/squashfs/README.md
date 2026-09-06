# Deterministic SquashFS

`squashfs.cpp` writes the SquashFS 4 image shared by AppImage and Snap projections. File data is
split into 128 KiB blocks and compressed with the pinned deterministic zstd settings when that
reduces its size. Inodes, directories, IDs, modes, and timestamps are written directly in sorted
order; metadata blocks remain uncompressed so their references do not depend on compressor output.

The writer omits fragments, extended attributes, duplicate-file aliases, device nodes, and export
tables. It accepts regular files from a checked install tree, creates their parent directories,
normalises directory and file modes, assigns UID and GID zero, and rejects unsafe or duplicate
paths. Coroutine callers run it through `runtime::offload`.
