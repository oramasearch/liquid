#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 <build-directory> <seed|baseline|retry>" >&2
  exit 2
fi

build_dir=$1
phase=$2
repo_root=$PWD
liquid="$repo_root/$build_dir/liquid"
proof="$repo_root/$build_dir/worker-proof"
project="$proof/project"
cache="$proof/cache"

run_wrap() {
  local selected_cache=$1
  shift
  (
    cd "$project"
    LIQUID_WRAP_CACHE="$selected_cache" "$liquid" wrap --no-verify "$@"
  )
}

if [[ $phase == seed ]]; then
  rm -rf "$proof"
  mkdir -p "$project"
  cp -R tests/workers/fixture/. "$project"
  clang-19 -O2 "$project/slow_copy.c" -o "$project/slow-copy"

  for sample in 1 2 3 4; do
    echo "worker sample $sample" >"$project/input.txt"
    make -C "$project" clean >/dev/null
    run_wrap "$cache" make
  done

  echo "worker baseline" >"$project/input.txt"
  make -C "$project" clean >/dev/null
  run_wrap "$proof/local-cache" make
  sha256sum "$project/output.o" | cut -d ' ' -f 1 >"$proof/local.sha256"
  cp -R "$cache" "$proof/retry-cache"

  exit 0
fi

if [[ $phase == baseline ]]; then
  make -C "$project" clean >/dev/null
  run_wrap "$cache" --scheduler 127.0.0.1:50051 --remote-deadline-ms 120000 make
  sha256sum "$project/output.o" | cut -d ' ' -f 1 >"$proof/remote.sha256"
  (
    cd "$project"
    LIQUID_WRAP_CACHE="$cache" "$liquid" why output.o --json >"$proof/remote-why.json"
  )
  cmp "$proof/local.sha256" "$proof/remote.sha256"
  grep -q '"decision":"remote"' "$proof/remote-why.json"
  echo "local output digest: $(<"$proof/local.sha256")"
  echo "remote output digest: $(<"$proof/remote.sha256")"
  echo "cost-model why: $(<"$proof/remote-why.json")"
  exit 0
fi

if [[ $phase == retry ]]; then
  echo "worker retry" >"$project/input.txt"
  make -C "$project" clean >/dev/null
  run_wrap "$proof/retry-cache" --scheduler 127.0.0.1:50051 \
    --remote-deadline-ms 120000 make
  sha256sum "$project/output.o" | cut -d ' ' -f 1 >"$proof/retry.sha256"
  sha256sum "$project/input.txt" | cut -d ' ' -f 1 >"$proof/retry-input.sha256"
  cmp "$proof/retry-input.sha256" "$proof/retry.sha256"
  echo "retry output digest: $(<"$proof/retry.sha256")"
  exit 0
fi

echo "unknown phase: $phase" >&2
exit 2
