#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: run-flatpak-interop.sh <liquid-tests> <output-directory>" >&2
    exit 2
fi

tests=$1
output=$2

for program in flatpak flatpak-builder; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "SKIP: $program is unavailable" >&2
        exit 77
    fi
done

for runtime in org.freedesktop.Platform org.freedesktop.Sdk; do
    if ! flatpak info "$runtime//24.08" >/dev/null 2>&1; then
        echo "SKIP: $runtime branch 24.08 is not installed" >&2
        exit 77
    fi
done

mkdir -p "$output"
LIQUID_PROJECTION_PEER_OUT="$output" \
    "$tests" --gtest_filter=Projection.emit_peer_artifacts_when_requested \
    --gtest_color=no

root="$output/flatpak/liquid-fixture-1.2.3.flatpak"
temporary=$(mktemp -d)
cleanup() {
    rm -rf "$temporary"
}
trap cleanup EXIT INT TERM

flatpak build-export "$temporary/repository" "$root"
flatpak-builder --build-only --force-clean \
    "$temporary/build" "$root/manifest.json"

echo "Flatpak build-export and flatpak-builder --build-only passed"
