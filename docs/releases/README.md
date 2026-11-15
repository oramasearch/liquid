# Releases

One page per tagged release, newest first. A page states what the release includes, what it
does not, what is knowingly still broken, and how it was verified.

- [v0.2.0](v0.2.0.md) — 2026-10-17. The library is the `v0.1.0` source: the release adds a test
  that holds the version in its three places, and moves the release procedure to `dev` → `main`.
  Lake `v0.2.0` is built from liquid `v0.1.0`.
- [v0.1.0](v0.1.0.md) — 2026-10-12. The first tagged release, so the page is the full inventory:
  every implemented feature by area, following the [implementation status](../library/status.md),
  and all 80 commands. Lake `v0.1.0` is built from this source.

## Conventions

- **Cut a release from `dev`, promote it to `main`.** The version is `current_version()` in
  `include/liquid/version.hpp`, `version_string()` in `src/version.cpp` and
  `project(liquid VERSION ...)` in `CMakeLists.txt`; move all three in the pull request into `dev`
  that adds the release page. `Version.StringMatchesStruct`, `Version.MatchesProjectVersion` and the
  ctest `release-notes-page` fail when they disagree or the page is missing.
- **Tag the merge commit** of the release pull request (`dev` into `main`) `vX.Y.Z` and publish the
  GitHub release from the page.
- **A first release, or one after a long gap, lists everything in it**, not a summary. Check the
  command list against `docs/reference/cli/cli.json`: every command is on the page, and the page
  names none the reference lacks.
- **Implemented is not specified.** A release page lists what
  [`docs/library/status.md`](../library/status.md) says is implemented. A feature specification
  under `docs/features/` is not a claim that the feature shipped.
- **Every number is measured** and says what it was measured on. A figure with no run behind it
  does not go in.
- **Known issues are part of the release**, linked to their tracking issue.
- Cross-repository references are full URLs; issues in this repository are linked by number.
