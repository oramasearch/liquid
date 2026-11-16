#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

# Runs a sanitizer, fuzzing or hardened-release build of a liquid checkout on a BuildKit daemon
# (linux/amd64), using the Dockerfile beside this script. Nothing is pushed. (liquid #407)
#
# usage: [MODE=ctest|fuzz] [CMAKE_OPTIONS=...] [BUILD_TYPE=Release] [FUZZ_SECONDS=60]
#        [FUZZ_HARNESSES="cbor sync"] [CTEST_JOBS=4] [BUILDKIT_ADDR=...] run.sh <checkout> <label>
#
# BUILDKIT_ADDR is a buildctl --addr. When unset, the first pod of the `buildkit` namespace in the
# kubectl context ${BUILDKIT_CONTEXT:-orama-production-cluster-01} is used, as liquid-bridge's
# Linux lane does. Only one run at a time: the BuildKit pod is shared and memory-limited.
# The log is written to ${LOG_DIRECTORY:-./hardening-logs}/<label>.log; a summary is printed.
set -u
if [ $# -ne 2 ]; then
  sed -n '2,12p' "$0"
  exit 2
fi
checkout=$(cd "$1" && pwd)
label=$2
here=$(cd "$(dirname "$0")" && pwd)
logs=${LOG_DIRECTORY:-$PWD/hardening-logs}
context=$(mktemp -d "${TMPDIR:-/tmp}/liquid-hardening-$label.XXXXXX")
mkdir -p "$logs"
trap 'rm -rf "$context"' EXIT
rsync -a --delete --exclude .git --exclude '/build*' --exclude .dockerignore "$checkout"/ "$context"/
if [ -z "${BUILDKIT_ADDR:-}" ]; then
  kube_context=${BUILDKIT_CONTEXT:-orama-production-cluster-01}
  pod=$(kubectl --context "$kube_context" get pods -n buildkit -o jsonpath='{.items[0].metadata.name}')
  BUILDKIT_ADDR="kube-pod://$pod?context=$kube_context&namespace=buildkit"
fi
buildctl --addr "$BUILDKIT_ADDR" build --frontend dockerfile.v0 \
  --local context="$context" --local dockerfile="$here" \
  --opt "build-arg:MODE=${MODE:-ctest}" --opt "build-arg:LABEL=$label" \
  --opt "build-arg:BUILD_TYPE=${BUILD_TYPE:-Release}" \
  --opt "build-arg:CMAKE_OPTIONS=${CMAKE_OPTIONS:-}" \
  --opt "build-arg:CTEST_JOBS=${CTEST_JOBS:-4}" \
  --opt "build-arg:FUZZ_SECONDS=${FUZZ_SECONDS:-60}" \
  --opt "build-arg:FUZZ_HARNESSES=${FUZZ_HARNESSES:-}" \
  --opt platform=linux/amd64 --progress plain > "$logs/$label.log" 2>&1
echo "exit=$? log=$logs/$label.log"
grep -E "CONFIGURE_EXIT|BUILD_EXIT|CTEST_EXIT|tests passed|tests failed|FAILED_TEST|SKIPPED_COUNT|^#[0-9]+ [0-9.]+ FUZZ|CRASH_INPUT|liquid: " \
  "$logs/$label.log" | sed -E 's/^#[0-9]+ [0-9.]+ //' | head -200
