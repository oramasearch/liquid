# SPDX-License-Identifier: MIT

ARG LIQUID_BASE=liquid-linux-ci
FROM ${LIQUID_BASE}

ARG LIQUID_BINARY=build-linux/liquid
COPY ${LIQUID_BINARY} /usr/local/bin/liquid

VOLUME ["/var/lib/liquid-node-store"]
# The control API (evict, budget) stays on loopback: it is unauthenticated (L-11, liquid#393).
# Nothing is exposed; mounts use the Unix socket.
ENTRYPOINT ["/usr/local/bin/liquid", "node-store", "serve"]
CMD ["--root", "/var/lib/liquid-node-store", "--listen", "unix:///run/liquid/node-store.sock", "--control-listen", "127.0.0.1:8082"]
