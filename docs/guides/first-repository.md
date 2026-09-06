# Your first repository

A drip is one captured write to disk. Drips are automatic and continuous; `cut` gives the current
aggregation a reviewable name as a change. The commands below create a repository, let the checkout
daemon capture an ordinary editor-style write, compare two named changes, search the working tree,
and push to a local `serve sync` peer.

<!-- guides-run: first-repository -->
```sh
mkdir source peer
printf 'first line\n' > source/notes.txt
(cd peer && liquid init .)
cd source
liquid init .
initial=$(liquid cut 'initial files')
initial=${initial#change=}
before=$(liquid log --ops --json | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
liquid status >/dev/null
printf 'second line\n' >> notes.txt
for attempt in $(seq 1 200); do
    after=$(liquid log --ops --json | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
    test "$after" -gt "$before" && break
    sleep 0.02
done
test "$after" -gt "$before"
changed=$(liquid cut 'add the second line')
changed=${changed#change=}
liquid log
liquid diff "$initial" "$changed"
liquid search 'second line'
port=${LIQUID_GUIDE_PORT:-18082}
liquid serve sync ../peer --listen 127.0.0.1 --port "$port" > peer.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; liquid daemon --shutdown >/dev/null 2>&1 || true' EXIT
python3 - "$port" <<'PY'
import socket
import sys
import time

port = int(sys.argv[1])
for _ in range(200):
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
            break
    except OSError:
        time.sleep(0.02)
else:
    raise SystemExit("sync peer did not start")
PY
liquid push "http://127.0.0.1:$port"
kill "$server_pid"
wait "$server_pid" || true
liquid daemon --shutdown
trap - EXIT
```

`init` imports regular files already present. `status` attaches to the daemon on macOS and Linux;
that process owns watcher capture. `log --ops` shows the signed drips, while `log` shows the named
changes. `diff` follows repository node identity across moves and reports content, mode, and
conflict changes. Search results come from the repository-local index.

The peer example has no authentication and listens only on loopback. A network service should use
the public sync server with its authorization callback. `LIQUID_TOKEN` supplies an optional bearer
credential to `push` and `pull`; liquid never prints it.
