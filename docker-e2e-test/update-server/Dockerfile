FROM golang:1.25-bookworm AS build

# Needed to fetch the repo's Git-LFS-tracked UI assets (e.g. server/ui/web/templates' embedded
# CSS) -- without them, go:embed happily embeds the raw LFS pointer-stub text instead of real
# content, and the binary panics at startup (not build time) the first time anything touches the
# UI templates package.
RUN apt-get update && apt-get install -y --no-install-recommends git-lfs \
    && rm -rf /var/lib/apt/lists/* && git lfs install

WORKDIR /build
# Pinned to a commit on foundriesio/update-server's main rather than a tagged release: the e2e
# support targets the post-#277 API (the tag is gone from rollout creation and from the storage
# addressing), which no release carries yet.
ARG UPDATE_SERVER_REF=208e846d50febb6024953ff5a0079b45b900a58b
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

# Once a release carries the post-#277 API, replace the build stage above with:
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
