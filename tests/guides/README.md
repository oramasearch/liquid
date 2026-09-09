# Guide tests

`run.py` finds every `<!-- guides-run: name -->` shell block below `docs/guides/`, refuses an
unmarked `sh` fence, and executes each marked block in an isolated directory. The Linux CTest uses
the built `liquid`, the Phase 2 CMake and Autotools fixtures, and an isolated home and cache.
It skips only when `/dev/fuse` is unavailable.

The runner reserves a loopback port only for a block that references `LIQUID_GUIDE_PORT`.
Blocks that need no network, including package resolution, run without opening a socket under the
offline sandbox. The first-repository guide still receives a port for its local sync peer.

`--guide resolve-packages.md` runs the source declaration and canonical-record workflow without
FUSE. The `guides-resolution` CTest runs it on macOS and Linux; the full Linux guide run retains
its FUSE prerequisite.
