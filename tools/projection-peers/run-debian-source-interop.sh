#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <liquid-tests> <output-directory>" >&2
  exit 2
fi

test_binary=$1
output=$2

for tool in debuild dpkg lintian sbuild; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required Debian peer tool is unavailable: $tool" >&2
    exit 1
  fi
done

mkdir -p "$output"
output=$(cd "$output" && pwd)
LIQUID_DEBIAN_SOURCE_PEER_OUT="$output" \
  "$test_binary" --gtest_filter=Projection.emit_debian_source_peer_when_requested

dsc=$(find "$output" -maxdepth 1 -name '*.dsc' -print -quit)
tree=$(find "$output" -mindepth 1 -maxdepth 1 -type d -name 'liquid-source-fixture-*' -print -quit)
if [[ -z "$dsc" || -z "$tree" ]]; then
  echo "source projection did not emit the expected tree and .dsc" >&2
  exit 1
fi

(
  cd "$output"
  sbuild --dist=bookworm --no-run-lintian "$dsc"
)

changes=$(find "$output" -maxdepth 1 -name '*_amd64.changes' -print -quit)
package=$(find "$output" -maxdepth 1 -name '*_amd64.deb' -print -quit)
if [[ -z "$changes" || -z "$package" ]]; then
  echo "sbuild did not emit an amd64 changes file and binary package" >&2
  exit 1
fi

lintian_output="$output/lintian.txt"
lintian -EvIL +pedantic "$changes" 2>&1 | tee "$lintian_output"
if grep -Eq '^[EW]:' "$lintian_output"; then
  echo "lintian reported an error or warning" >&2
  exit 1
fi

(
  cd "$tree"
  debuild -S -us -uc
)

dpkg -i "$package"
test "$(liquid-source-fixture)" = "liquid source fixture"

echo "debuild-source: accepted"
echo "sbuild: accepted"
echo "lintian-errors-warnings: 0"
echo "installed-command: liquid source fixture"
