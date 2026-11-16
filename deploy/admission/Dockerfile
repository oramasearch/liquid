# SPDX-License-Identifier: MIT

ARG LIQUID_BASE=liquid-linux-ci
FROM ${LIQUID_BASE}

ARG LIQUID_BINARY=build-linux/liquid
COPY ${LIQUID_BINARY} /usr/local/bin/liquid

VOLUME ["/var/lib/liquid-admission"]
EXPOSE 8443 8083
ENTRYPOINT ["/usr/local/bin/liquid", "serve", "admission"]
CMD ["--policy", "/etc/liquid/admission.json", "--listen", "0.0.0.0:8443", "--control-listen", "0.0.0.0:8083", "--tls-cert", "/etc/liquid/tls/tls.crt", "--tls-key", "/etc/liquid/tls/tls.key", "--store", "/var/lib/liquid-admission"]
