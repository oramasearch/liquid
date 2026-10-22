#!/usr/bin/env bash
# Inspect already-built images; no build, push, host mount, or elevated container is used.
set -euo pipefail
if [[ $# != 1 ]]; then
  echo "usage: $0 <image-prefix> (expects PREFIX-{admission,node-store,scheduler,worker}:test)" >&2
  exit 2
fi
for service in admission node-store scheduler worker; do
  image="$1-$service:test"
  test "$(docker image inspect --format '{{.Config.User}}' "$image")" = 65532:65532
  timeout 60s docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
    --env "LIQUID_TEST_SERVICE=$service" \
    --entrypoint /bin/sh "$image" -ec '
      test "$(id -u):$(id -g)" = 65532:65532
      test "$(id -un):$(id -gn)" = liquid:liquid
      test "$(getent passwd 65532 | cut -d: -f6-7)" = /var/lib/liquid-identity:/usr/sbin/nologin
      test "$(stat -c %u:%g:%a /usr/local/bin/liquid)" = 0:0:555
      ! test -w /usr/local/bin/liquid
      test "$LIQUID_HOME" = /var/lib/liquid-identity
      test "$(stat -c %u:%g:%a "$LIQUID_HOME")" = 65532:65532:700
      touch "$LIQUID_HOME/permission-check"
      rm "$LIQUID_HOME/permission-check"
      store="/var/lib/liquid-$LIQUID_TEST_SERVICE"
      test "$(stat -c %u:%g:%a "$store")" = 65532:65532:700
      touch "$store/permission-check"
      rm "$store/permission-check"
      if test "$LIQUID_TEST_SERVICE" = node-store || test "$LIQUID_TEST_SERVICE" = worker; then
        test -x /usr/bin/fusermount3
      fi
      if test "$LIQUID_TEST_SERVICE" = node-store; then
        test "$(stat -c %u:%g:%a /run/liquid)" = 65532:65532:755
        touch /run/liquid/permission-check
        rm /run/liquid/permission-check
      fi
      if test "$LIQUID_TEST_SERVICE" = worker; then
        test "$(stat -c %u:%g:%a /usr/local/libexec/liquid/liquid-read-guard)" = 0:0:555
        test -x /usr/bin/unshare
        test -x /usr/bin/mount
      fi
      # Search all executable locations, including versioned and cross compiler names.
      paths=$(find /usr /opt /bin /sbin \( -type f -o -type l \) -print)
      found=$(printf "%s\n" "$paths" | \
        sed "s|.*/||" | \
        grep -E "^((.*-)?(gcc|g\+\+|clang|clang\+\+|cc|c\+\+)(-[0-9]+)?|cc1|cc1plus|cmake|ninja|make)$" || true)
      test -z "$found" || { echo "unexpected build tools: $found" >&2; exit 1; }
      liquid version
      liquid identity show --document >/dev/null
    '
  echo "$image: non-root, runtime-only, binary modes and writable state verified"
done
