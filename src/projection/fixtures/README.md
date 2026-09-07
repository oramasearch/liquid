# Debian projection fixtures

`debian-bookworm-path-packages.tsv` is derived output, not a hand-maintained package-name map.
The projection peer runs `dpkg-query -S` for each tool path in the pinned Debian bookworm image
and records the normalized package name beside the exact query output. The source-package writer
matches only rows present here; an absent path is reported instead of guessed.

The `policy` column marks packages supplied by Debian's required build environment as `implicit`;
all other mapped packages are `explicit` build dependencies. The refresh script owns that policy
classification so regenerated output remains reviewable.

Regenerate the table with `tools/projection-peers/refresh-debian-path-packages.sh` in that peer and
review the image digest, paths, package names, and raw query output together.
