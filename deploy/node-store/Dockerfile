FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libssl3t64 zlib1g libfuse3-3 libpcsclite1 fuse3 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 65532 liquid \
 && useradd --system --uid 65532 --gid 65532 --no-create-home --no-log-init --home-dir /var/lib/liquid-identity --shell /usr/sbin/nologin --key SYS_UID_MAX=65532 liquid \
 && install -d -o 65532 -g 65532 -m 0700 /var/lib/liquid-node-store /var/lib/liquid-identity \
 && install -d -o 65532 -g 65532 -m 0755 /run/liquid

ARG LIQUID_BINARY=build-linux/liquid
COPY --chmod=0555 ${LIQUID_BINARY} /usr/local/bin/liquid

ENV LIQUID_HOME=/var/lib/liquid-identity
VOLUME ["/var/lib/liquid-node-store", "/var/lib/liquid-identity"]
USER 65532:65532
EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/liquid", "node-store", "serve"]
CMD ["--root", "/var/lib/liquid-node-store", "--listen", "unix:///run/liquid/node-store.sock", "--control-listen", "127.0.0.1:8082"]
