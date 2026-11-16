# AGENTS.md

Read `CLAUDE.md` first. It is the instruction file for this repository and applies in full to
every coding agent. Then `README.md` and `docs/README.md`.

Non-negotiables, restated from `CLAUDE.md`:

- Every change documents itself in `docs/<topic>/*.md` **in the same commit**, and updates the
  status in `docs/library/status.md` when it stops being true. The root `README.md` is a front
  door and carries only a short status.
- **No AI attribution anywhere.** No `Co-Authored-By`, no "Generated with", no session or
  model trailers, in commits or pull requests.
- No time estimates. The word "seam" is banned.
- Work on a feature branch from `dev`, open the pull request against `dev`, and never push to
  `main` directly.
- Never take a dependency on Zetta or on Lake.
- Build and test: `cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build`.
