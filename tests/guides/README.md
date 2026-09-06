# Guide tests

`run.py` finds every `<!-- guides-run: name -->` shell block below `docs/guides/`, refuses an
unmarked `sh` fence, and executes each marked block in an isolated directory. The Linux CTest uses
the built `liquid`, the Phase 2 CMake and Autotools fixtures, an isolated home and cache, and an
available loopback port. It skips only when `/dev/fuse` is unavailable.
