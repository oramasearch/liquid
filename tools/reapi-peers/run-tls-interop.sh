#!/usr/bin/env bash
set -euo pipefail

if ! command -v docker >/dev/null 2>&1; then
  echo "SKIP: Docker is unavailable; TLS peer interoperability was not run"
  exit 0
fi

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <liquid-tests build directory>" >&2
  exit 2
fi

build_dir=$1
peer_name=liquid-155-nativelink-tls
server_log=tools/reapi-peers/liquid-http2-test-server.log
certificate=tools/reapi-peers/nativelink-tls-cert.pem
private_key=tools/reapi-peers/nativelink-tls-key.pem

if [[ $(uname) == Darwin ]]; then
  peer_network=(-p 50053:50053)
  grpc_host=host.docker.internal:50052
else
  peer_network=(--network host)
  grpc_host=127.0.0.1:50052
fi

cleanup() {
  docker rm -f "$peer_name" >/dev/null 2>&1 || true
  if [[ -n ${server_pid:-} ]]; then
    kill "$server_pid" >/dev/null 2>&1 || true
  fi
  rm -f "$server_log"
}
trap cleanup EXIT

docker rm -f "$peer_name" >/dev/null 2>&1 || true
docker run -d --rm --name "$peer_name" "${peer_network[@]}" \
  -v "$PWD/tools/reapi-peers/nativelink-tls.json5:/config:ro" \
  -v "$PWD/tools/reapi-peers:/certs:ro" \
  ghcr.io/tracemachina/nativelink:v1.6.6 /config >/dev/null

for _ in $(seq 1 20); do
  if docker logs "$peer_name" 2>&1 | grep -q "Ready, listening"; then
    break
  fi
  sleep 1
done
if ! docker logs "$peer_name" 2>&1 | grep -q "Ready, listening"; then
  docker logs "$peer_name" >&2
  exit 1
fi

LIQUID_HTTP2_NATIVE_LINK_TLS=1 "$build_dir/tests/liquid-tests" \
  --gtest_filter=Http2Interop.GetsCapabilitiesFromNativeLinkOverTls

"$build_dir/tests/liquid-http2-test-server" --tls "$certificate" "$private_key" \
  >"$server_log" 2>&1 &
server_pid=$!
for _ in $(seq 1 20); do
  if grep -q READY "$server_log"; then
    break
  fi
  sleep 1
done
if ! grep -q READY "$server_log"; then
  cat "$server_log" >&2
  exit 1
fi

if [[ $(uname) == Darwin ]]; then
  docker run --rm -v "$PWD/tools/reapi-peers:/protos:ro" \
    fullstorydev/grpcurl:latest -insecure -import-path /protos -proto liquid_echo.proto \
    -d '{"value":"tls"}' "$grpc_host" liquid.test.Echo/Call
else
  docker run --rm --network host -v "$PWD/tools/reapi-peers:/protos:ro" \
    fullstorydev/grpcurl:latest -insecure -import-path /protos -proto liquid_echo.proto \
    -d '{"value":"tls"}' "$grpc_host" liquid.test.Echo/Call
fi

wait "$server_pid"
server_pid=
