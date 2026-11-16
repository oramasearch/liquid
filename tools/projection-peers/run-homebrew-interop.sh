#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: run-homebrew-interop.sh <liquid-tests> <native-binary> <output-directory>" >&2
    exit 2
fi

tests=$1
native_binary=$2
output=$3
tap=liquid/issue-77-peer
formula=liquid-homebrew-fixture
port=18077

if ! command -v brew >/dev/null 2>&1; then
    echo "SKIP: Homebrew is unavailable" >&2
    exit 77
fi

mkdir -p "$output"
LIQUID_HOMEBREW_NATIVE_BINARY="$native_binary" \
LIQUID_HOMEBREW_PEER_OUT="$output" \
    "$tests" --gtest_filter=Projection.emit_native_homebrew_peer_when_requested \
    --gtest_color=no

tap_root=
if brew tap | grep -Fx "$tap" >/dev/null 2>&1; then
    tap_root=$(brew --repository "$tap")
fi
installed=0
server_pid=
cleanup() {
    if [ "$installed" -eq 1 ]; then
        brew uninstall --force "$formula" >/dev/null 2>&1 || true
    fi
    if [ -n "$server_pid" ]; then
        kill "$server_pid" >/dev/null 2>&1 || true
        wait "$server_pid" 2>/dev/null || true
    fi
    if [ -n "$tap_root" ]; then
        brew untap "$tap" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT INT TERM

if [ -n "$tap_root" ]; then
    brew untap "$tap" >/dev/null
fi
brew tap-new "$tap" >/dev/null
tap_root=$(brew --repository "$tap")
mkdir -p "$tap_root/Formula"
cp "$output/$formula.rb" "$tap_root/Formula/$formula.rb"

ruby -run -e httpd "$output" -p "$port" >/dev/null 2>&1 &
server_pid=$!
HOMEBREW_NO_AUTO_UPDATE=1 \
HOMEBREW_BOTTLE_DOMAIN="http://127.0.0.1:$port" \
    brew install --formula --force-bottle "$tap/$formula"
installed=1
brew test "$tap/$formula"
echo "Homebrew bottle install and formula test passed"
