FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        openssl \
    && rm -rf /var/lib/apt/lists/*

# Pre-1.0: pin a release, and keep fiocli in ../Dockerfile on the same one.
ARG UPDATE_SERVER_VERSION=v0.9.5
ARG TARGETARCH=amd64
RUN curl -fsSL -o /usr/local/bin/fioserver \
        "https://github.com/foundriesio/update-server/releases/download/${UPDATE_SERVER_VERSION}/fioserver-linux-${TARGETARCH}" \
    && chmod +x /usr/local/bin/fioserver

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# 8080: web UI + user-facing management API. 8443: mTLS device gateway API.
EXPOSE 8080 8443

ENTRYPOINT ["/entrypoint.sh"]
