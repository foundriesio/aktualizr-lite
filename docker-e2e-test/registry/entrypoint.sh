#!/bin/sh -e
#
# Wraps the stock registry:2 entrypoint to self-issue a TLS cert for this
# service's own hostname on first run.
#
# Why this is needed: `composectl publish` and `composectl pull` disagree on
# HTTP vs HTTPS for a registry. `publish` (github.com/docker/docker/registry)
# auto-classifies "localhost"/"127.0.0.1" as insecure/HTTP with no override;
# `pull` (containerd's docker resolver) always requires real HTTPS, including
# for "localhost", with no insecure-registry option wired up in composeapp.
# No single host/protocol combination against a bare, unauthenticated
# registry satisfies both. Using a non-loopback hostname (this service's own
# compose DNS name) with a real -- even if self-signed -- cert makes both
# tools agree on plain HTTPS, as long as the cert's CA is trusted wherever
# `docker build/push` and `composectl publish/pull` run (see dockerd's and
# aklite-e2e-test's own startup in docker-compose.yml / entrypoint.sh).
#
# The generated cert is self-signed and doubles as its own trust anchor. It
# is written to a volume shared with those other containers.

HOSTNAME=${E2E_REGISTRY_HOSTNAME:-registry}
CERT_DIR=$(dirname "$REGISTRY_HTTP_TLS_CERTIFICATE")
mkdir -p "$CERT_DIR"

if [ ! -f "$REGISTRY_HTTP_TLS_CERTIFICATE" ]; then
    echo "## Generating self-signed TLS cert for $HOSTNAME ..."
    openssl req -x509 -newkey rsa:2048 -nodes \
        -keyout "$REGISTRY_HTTP_TLS_KEY" \
        -out "$REGISTRY_HTTP_TLS_CERTIFICATE" \
        -days 3650 \
        -subj "/CN=${HOSTNAME}" \
        -addext "subjectAltName=DNS:${HOSTNAME}"
fi

exec /entrypoint.sh "$@"
