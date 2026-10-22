FROM ubuntu:24.04

RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libstdc++6 libssl3t64 zlib1g libfuse3-3 libpcsclite1 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 65532 liquid \
 && useradd --system --uid 65532 --gid 65532 --no-create-home --no-log-init --home-dir /var/lib/liquid-identity --shell /usr/sbin/nologin --key SYS_UID_MAX=65532 liquid \
 && install -d -o 65532 -g 65532 -m 0700 /var/lib/liquid-admission /var/lib/liquid-identity

ARG LIQUID_BINARY=build-linux/liquid
COPY --chmod=0555 ${LIQUID_BINARY} /usr/local/bin/liquid

ENV LIQUID_HOME=/var/lib/liquid-identity
VOLUME ["/var/lib/liquid-admission", "/var/lib/liquid-identity"]
USER 65532:65532
EXPOSE 8443 8083
ENTRYPOINT ["/usr/local/bin/liquid", "serve", "admission"]
CMD ["--policy", "/etc/liquid/admission.json", "--listen", "0.0.0.0:8443", "--control-listen", "127.0.0.1:8083", "--tls-cert", "/etc/liquid/tls/tls.crt", "--tls-key", "/etc/liquid/tls/tls.key", "--store", "/var/lib/liquid-admission"]
