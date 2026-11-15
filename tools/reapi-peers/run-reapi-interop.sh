#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: Docker is unavailable; REAPI interoperability was not run"
  exit 0
fi

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <build directory>" >&2
  exit 2
fi

repo_root=$PWD
build_dir=$1
tests="$repo_root/$build_dir/tests/liquid-tests"
liquid="$repo_root/$build_dir/liquid"
temporary=$(mktemp -d)
native_name="liquid-reapi-native-$$"
buildbarn_name="liquid-reapi-buildbarn-$$"
server_pid=

if [[ $(uname) == Darwin ]]; then
  native_network=(-p 50051:50051)
  buildbarn_network=(-p 50052:50052)
  bazel_network=(--network bridge)
  server_host=host.docker.internal
else
  native_network=(--network host)
  buildbarn_network=(--network host)
  bazel_network=(--network host)
  server_host=127.0.0.1
fi

cleanup() {
  docker rm -f "$native_name" "$buildbarn_name" >/dev/null 2>&1 || true
  if [[ -n $server_pid ]]; then
    kill "$server_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$temporary"
}
trap cleanup EXIT

wait_for_peer() {
  local endpoint=$1
  local log=$2
  for _ in $(seq 1 30); do
    if LIQUID_REAPI_ENDPOINT="$endpoint" "$tests" \
      --gtest_filter=RemoteInterop.CasAndActionCacheRoundTripWithConfiguredPeer \
      >"$log" 2>&1; then
      cat "$log"
      return
    fi
    sleep 1
  done
  cat "$log" >&2
  return 1
}

docker run -d --rm --name "$native_name" "${native_network[@]}" \
  -v "$repo_root/tools/reapi-peers/nativelink.json5:/config:ro" \
  ghcr.io/tracemachina/nativelink:v1.6.6 /config >/dev/null
wait_for_peer http://127.0.0.1:50051 "$temporary/nativelink-client.log"
if [[ $(uname) == Linux ]]; then
  LIQUID_REAPI_ENDPOINT=http://127.0.0.1:50051 "$tests" \
    --gtest_filter=BuildWrap.reapi_action_cache_matches_the_local_second_run_hit_rate
  echo "NativeLink second run: local=100% remote=100%"
fi

mkdir -p "$temporary/storage-cas/state" "$temporary/storage-ac/state"
docker run -d --rm --name "$buildbarn_name" "${buildbarn_network[@]}" \
  -v "$repo_root/tools/reapi-peers/buildbarn.jsonnet:/config.jsonnet:ro" \
  -v "$temporary/storage-cas:/storage-cas" \
  -v "$temporary/storage-ac:/storage-ac" \
  ghcr.io/buildbarn/bb-storage:20260609T094425Z-10acc76 /config.jsonnet >/dev/null
wait_for_peer http://127.0.0.1:50052 "$temporary/buildbarn-client.log"
if [[ $(uname) == Linux ]]; then
  LIQUID_REAPI_ENDPOINT=http://127.0.0.1:50052 "$tests" \
    --gtest_filter=BuildWrap.reapi_action_cache_matches_the_local_second_run_hit_rate
  "$tests" --gtest_filter=Build.refuses_non_hermetic_read_for_remote_eligible_action
  echo "BuildBarn second run: local=100% remote=100%"
  echo "Non-hermetic read: refused locally before remote dispatch"
fi

mkdir -p "$temporary/server/.liquid-store"
(
  cd "$temporary/server"
  "$liquid" serve reapi --listen 0.0.0.0:50161
) >"$temporary/server.log" 2>&1 &
server_pid=$!
for _ in $(seq 1 30); do
  if (exec 3<>/dev/tcp/127.0.0.1/50161) 2>/dev/null; then
    break
  fi
  sleep 1
done
if ! (exec 3<>/dev/tcp/127.0.0.1/50161) 2>/dev/null; then
  cat "$temporary/server.log" >&2
  exit 1
fi

cp -R "$repo_root/tools/reapi-peers/bazel-fixture" "$temporary/bazel-write"
if ! write_output=$(docker run --rm "${bazel_network[@]}" \
  -v "$temporary/bazel-write:/workspace" -w /workspace \
  gcr.io/bazel-public/bazel:7.4.1 \
  --batch build //:copy --enable_bzlmod=false --enable_workspace=true \
  --remote_cache="grpc://$server_host:50161" \
  --remote_max_connections=1 \
  --remote_upload_local_results=true 2>&1); then
  echo "$write_output" >&2
  cat "$temporary/server.log" >&2
  exit 1
fi
echo "$write_output"

cp -R "$repo_root/tools/reapi-peers/bazel-fixture" "$temporary/bazel-read"
if ! read_output=$(docker run --rm "${bazel_network[@]}" \
  -v "$temporary/bazel-read:/workspace" -w /workspace \
  gcr.io/bazel-public/bazel:7.4.1 \
  --batch build //:copy --enable_bzlmod=false --enable_workspace=true \
  --remote_cache="grpc://$server_host:50161" \
  --remote_max_connections=1 \
  --remote_upload_local_results=false 2>&1); then
  echo "$read_output" >&2
  cat "$temporary/server.log" >&2
  exit 1
fi
echo "$read_output"
if ! grep -q "1 remote cache hit" <<<"$read_output"; then
  echo "Bazel did not report a remote cache hit from liquid" >&2
  cat "$temporary/server.log" >&2
  exit 1
fi
echo "Bazel stock-client CAS and ActionCache read/write: PASS"
