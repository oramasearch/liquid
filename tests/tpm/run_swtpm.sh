#!/bin/sh
set -eu

test_binary=$1
swtpm_binary=$2
state_directory=$(mktemp -d "${TMPDIR:-/tmp}/liquid-swtpm.XXXXXX")
command_socket=$state_directory/command.sock
control_socket=$state_directory/control.sock
swtpm_pid=

cleanup() {
  if [ -n "$swtpm_pid" ]; then
    kill "$swtpm_pid" 2>/dev/null || true
    wait "$swtpm_pid" 2>/dev/null || true
  fi
  rm -rf "$state_directory"
}
trap cleanup EXIT INT TERM

"$swtpm_binary" socket \
  --tpm2 \
  --tpmstate "dir=$state_directory" \
  --server "type=unixio,path=$command_socket" \
  --ctrl "type=unixio,path=$control_socket" \
  --flags startup-clear &
swtpm_pid=$!

attempt=0
while [ ! -S "$command_socket" ]; do
  attempt=$((attempt + 1))
  if [ "$attempt" -ge 100 ]; then
    echo "swtpm command socket did not appear" >&2
    exit 1
  fi
  sleep 0.1
done

LIQUID_SWTPM_SOCKET=$command_socket \
  "$test_binary" --gtest_filter=TpmCustody.swtpm_seals_unseals_and_refuses_a_changed_pcr
