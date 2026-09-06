ARG LIQUID_BASE=liquid-linux-ci
FROM ${LIQUID_BASE}

ARG LIQUID_BINARY=build-linux/liquid
COPY ${LIQUID_BINARY} /usr/local/bin/liquid

VOLUME ["/var/lib/liquid-node-store"]
EXPOSE 8082
ENTRYPOINT ["/usr/local/bin/liquid", "node-store", "serve"]
CMD ["--root", "/var/lib/liquid-node-store", "--listen", "unix:///run/liquid/node-store.sock", "--control-listen", "0.0.0.0:8082"]
