#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <build-directory>" >&2
  exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required for the worker fleet proof" >&2
  exit 1
fi

repo_root=$PWD
build_dir=$1
runtime="$repo_root/$build_dir/worker-runtime"
token=$$
scheduler_image="liquid-scheduler-proof:$token"
worker_image="liquid-worker-proof:$token"
scheduler="liquid-scheduler-proof-$token"
worker_one="liquid-worker-one-proof-$token"
worker_two="liquid-worker-two-proof-$token"
client="liquid-client-proof-$token"

cleanup() {
  local status=$?
  if [[ $status -ne 0 ]]; then
    docker ps -a --filter "name=$scheduler" --filter "name=$worker_one" \
      --filter "name=$worker_two" --filter "name=$client" >&2 || true
    docker logs "$scheduler" >&2 || true
    docker logs "$worker_one" >&2 || true
    docker logs "$worker_two" >&2 || true
  fi
  docker rm -f "$client" "$worker_one" "$worker_two" "$scheduler" >/dev/null 2>&1 || true
  docker image rm -f "$worker_image" "$scheduler_image" >/dev/null 2>&1 || true
}
trap cleanup EXIT

wait_http() {
  local url=$1
  for _ in $(seq 1 120); do
    if docker exec "$scheduler" wget -T 1 -qO- "$url" >/dev/null 2>&1; then
      return
    fi
    sleep 0.1
  done
  docker logs "$scheduler" >&2 || true
  docker logs "$worker_one" >&2 || true
  docker logs "$worker_two" >&2 || true
  echo "readiness probe failed: $url" >&2
  return 1
}

read_scheduler_metrics() {
  local output
  for _ in $(seq 1 120); do
    if output=$(docker exec "$scheduler" wget -T 1 -qO- http://127.0.0.1:8080/metrics) &&
      grep -q 'liquid_scheduler_' <<<"$output"; then
      echo "$output"
      return
    fi
    sleep 0.1
  done
  docker logs "$scheduler" >&2 || true
  echo "scheduler metrics probe failed" >&2
  return 1
}

# liquid #416: each process has its own identity, and each side pins the other's document.
provision_identity() {
  local image=$1 name=$2 output=$3
  mkdir -p "$runtime/$name-home"
  docker run --rm -e LIQUID_HOME=/var/lib/liquid-identity \
    -v "$runtime/$name-home:/var/lib/liquid-identity" \
    --entrypoint /usr/local/bin/liquid "$image" identity show --document >"$output"
}

start_workers() {
  docker run -d --name "$worker_one" --network host --privileged --device /dev/fuse \
    -e LIQUID_HOME=/var/lib/liquid-identity \
    -v "$runtime/worker-one-home:/var/lib/liquid-identity" \
    -v "$runtime/trust:/etc/liquid:ro" \
    -v "$runtime/worker-one:/var/lib/liquid-worker" \
    "$worker_image" \
    --scheduler 127.0.0.1:50051 \
    --trust-scheduler /etc/liquid/scheduler \
    --control-listen 0.0.0.0:8081 \
    --poll-ms 2000 \
    --root /var/lib/liquid-worker >/dev/null
  docker run -d --name "$worker_two" --network host --privileged --device /dev/fuse \
    -e LIQUID_HOME=/var/lib/liquid-identity \
    -v "$runtime/worker-two-home:/var/lib/liquid-identity" \
    -v "$runtime/trust:/etc/liquid:ro" \
    -v "$runtime/worker-two:/var/lib/liquid-worker" \
    "$worker_image" \
    --scheduler 127.0.0.1:50051 \
    --trust-scheduler /etc/liquid/scheduler \
    --control-listen 0.0.0.0:8082 \
    --poll-ms 2000 \
    --root /var/lib/liquid-worker >/dev/null
  wait_http http://127.0.0.1:8081/readyz
  wait_http http://127.0.0.1:8082/readyz
}

start_client() {
  local phase=$1
  docker run -d --name "$client" --network host --privileged --device /dev/fuse \
    -v "$repo_root:/src" -w /src liquid-linux-ci \
    tests/workers/client-proof.sh "$build_dir" "$phase" >/dev/null
}

wait_client() {
  if ! docker wait "$client" | grep -q '^0$'; then
    docker logs "$client" >&2
    docker logs "$scheduler" >&2
    return 1
  fi
  docker logs "$client"
  return 0
}

docker build -q -t "$scheduler_image" \
  --build-arg "LIQUID_BINARY=$build_dir/liquid" \
  -f deploy/workers/Dockerfile.scheduler . >/dev/null
docker build -q -t "$worker_image" \
  --build-arg "LIQUID_BINARY=$build_dir/liquid" \
  --build-arg "LIQUID_READ_GUARD=$build_dir/liquid-read-guard" \
  -f deploy/workers/Dockerfile.worker . >/dev/null

docker run --rm --privileged --device /dev/fuse \
  -v "$repo_root:/src" -w /src liquid-linux-ci \
  tests/workers/client-proof.sh "$build_dir" seed

rm -rf "$runtime"
mkdir -p "$runtime/scheduler" "$runtime/worker-one" "$runtime/worker-two" \
  "$runtime/trust/workers"
provision_identity "$scheduler_image" scheduler "$runtime/trust/scheduler"
provision_identity "$worker_image" worker-one "$runtime/trust/workers/worker-one"
provision_identity "$worker_image" worker-two "$runtime/trust/workers/worker-two"
docker run -d --name "$scheduler" --network host \
  -e LIQUID_HOME=/var/lib/liquid-identity \
  -v "$runtime/scheduler-home:/var/lib/liquid-identity" \
  -v "$runtime/trust:/etc/liquid:ro" \
  -v "$runtime/scheduler:/var/lib/liquid-scheduler" \
  "$scheduler_image" \
  --listen 0.0.0.0:50051 \
  --trust-worker /etc/liquid/workers \
  --control-listen 0.0.0.0:8080 \
  --store /var/lib/liquid-scheduler \
  --lease-ms 8000 >/dev/null
wait_http http://127.0.0.1:8080/healthz

start_workers
start_client baseline
wait_client
docker rm "$client" >/dev/null
docker rm -f "$worker_one" "$worker_two" >/dev/null

start_workers
start_client retry

killed=
while [[ $(docker inspect -f '{{.State.Running}}' "$client") == true ]]; do
  if docker top "$worker_one" -eo pid,args 2>/dev/null | grep -q '/slow-copy'; then
    docker kill "$worker_one" >/dev/null
    killed=$worker_one
    break
  fi
  if docker top "$worker_two" -eo pid,args 2>/dev/null | grep -q '/slow-copy'; then
    docker kill "$worker_two" >/dev/null
    killed=$worker_two
    break
  fi
  sleep 0.05
done
if [[ -z $killed ]]; then
  docker logs "$client" >&2
  docker top "$worker_one" -eo pid,args >&2 || true
  docker top "$worker_two" -eo pid,args >&2 || true
  echo "no active worker was observed" >&2
  exit 1
fi

wait_client

metrics=$(read_scheduler_metrics)
if ! grep -q 'liquid_scheduler_requeued 1' <<<"$metrics"; then
  echo "$metrics" >&2
  exit 1
fi
echo "killed worker: $killed"
echo "$metrics"
echo "worker fleet Docker proof: PASS"
