FROM golang:1.25-bookworm AS build

# Needed to fetch the repo's Git-LFS-tracked UI assets (e.g. server/ui/web/templates' embedded
# CSS) -- without them, go:embed happily embeds the raw LFS pointer-stub text instead of real
# content, and the binary panics at startup (not build time) the first time anything touches the
# UI templates package.
RUN apt-get update && apt-get install -y --no-install-recommends git-lfs \
    && rm -rf /var/lib/apt/lists/* && git lfs install

WORKDIR /build
# Pinned to a commit on foundriesio/update-server's unmerged `add-rollout-interval-flag` branch,
# not a tagged release: adds a `--rolloutinterval` flag to `serve`, needed so a rollout created
# by e2e-test.py doesn't sit unapplied for up to 5 minutes (the otherwise-compiled-in default,
# server/ui/daemons's rolloutWatchdog). Switch back to fetching a tagged release binary (as
# below, commented out) once this ships in one.
ARG UPDATE_SERVER_REF=3258c3d8c2f5ff5ae36fd76c01d6694f76c11f5b
RUN git clone https://github.com/foundriesio/update-server.git && cd update-server \
    && git checkout ${UPDATE_SERVER_REF} \
    && git lfs pull \
    && go build -o /usr/local/bin/fioserver ./cmd/server


FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        openssl \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/bin/fioserver /usr/local/bin/fioserver

# Once `--rolloutinterval` ships in a tagged release, replace the build stage above with:
#   ARG UPDATE_SERVER_VERSION=vX.Y.Z
#   ARG TARGETARCH=amd64
#   RUN curl -fsSL -o /usr/local/bin/fioserver \
#           "https://github.com/foundriesio/update-server/releases/download/${UPDATE_SERVER_VERSION}/fioserver-linux-${TARGETARCH}" \
#       && chmod +x /usr/local/bin/fioserver

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# 8080: web UI + user-facing management API. 8443: mTLS device gateway API.
EXPOSE 8080 8443

ENTRYPOINT ["/entrypoint.sh"]
