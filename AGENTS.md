# AGENTS.md

Read `CLAUDE.md` first. It is the instruction file for this repository and applies in full to
every agent, Codex included. Then `README.md` and `docs/README.md`.

Non-negotiables, restated from `CLAUDE.md` and the org conventions in
[`oramasearch/orama-bridge`](https://github.com/oramasearch/orama-bridge):

- Every change documents itself in `docs/<topic>/*.md` **in the same commit**, and updates the
  status in `docs/library/status.md` and the status lines in `CLAUDE.md` when they stop being
  true. The root `README.md` is a front door and carries only a short status.
- **No AI attribution anywhere.** No `Co-Authored-By`, no "Generated with", no session or
  model trailers, in commits or pull requests.
- No time estimates. The word "seam" is banned.
- Work on a feature branch named for the issue, open a pull request that references the issue,
  and never push to `main` directly. Commit means push.
- Never take a dependency on Zetta or on `oramasearch/lake`; this repository is open source eventually.
- Work is tracked from the master epic https://github.com/oramasearch/liquid-bridge/issues/7;
  the structure is in the bridge's `docs/tracking.md`.
