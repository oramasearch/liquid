ARG LIQUID_BASE=liquid-linux-ci
FROM ${LIQUID_BASE}

ARG LIQUID_BINARY=build-linux/liquid
COPY ${LIQUID_BINARY} /usr/local/bin/liquid

VOLUME ["/var/lib/liquid-admission"]
# The control API (policy reload, decisions, /mcp) stays on loopback: it is unauthenticated
# (L-11, liquid#393). Only the webhook port is exposed.
EXPOSE 8443
ENTRYPOINT ["/usr/local/bin/liquid", "serve", "admission"]
CMD ["--policy", "/etc/liquid/admission.json", "--listen", "0.0.0.0:8443", "--control-listen", "127.0.0.1:8083", "--tls-cert", "/etc/liquid/tls/tls.crt", "--tls-key", "/etc/liquid/tls/tls.key", "--store", "/var/lib/liquid-admission"]
