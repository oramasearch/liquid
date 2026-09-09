# User guides

- [Wrap an existing build](wrap-an-existing-build.md) exercises real CMake and Autotools projects,
  cache reuse, miss explanation, and provenance.
- [Your first repository](first-repository.md) covers automatic drips, named changes, diff, search,
  and push to a local sync peer.
- [Keys and encryption](keys-and-encryption.md) starts with public identity export for sign-up,
  then describes encrypted paths, current key providers,
  rotation behavior, and custody work that has not landed.

Every shell block has a `guides-run` marker and runs through the Linux `guides-run` CTest against
the repository fixtures.

[Resolve packages from declarations](resolve-packages.md) checks a two-package dependency graph
and canonical record file/stdout output. It also runs natively on macOS through `guides-resolution`
without a mount or repository.

[Package ordinary repository files](project-from-files.md) records files, packages a change as
Debian and OCI artifacts, prints its root with `liquid tree`, and compares deterministic Debian
bytes. It runs natively through `guides-projection` without a filesystem mount.
