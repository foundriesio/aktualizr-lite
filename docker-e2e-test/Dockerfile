FROM golang:1.24.13-bookworm AS composeapp
# Build composeapp
WORKDIR /build
RUN git clone https://github.com/foundriesio/composeapp.git && cd composeapp \
    && git checkout main \
    && STOREROOT=/var/sota/reset-apps COMPOSEROOT=/var/sota/compose-apps BASESYSTEMCONFIG=/usr/lib/docker make \
    && cp ./bin/composectl /usr/bin/

# We may add fioconfig to the test sequence. For now, we don't use it
# WORKDIR /build
# RUN git clone https://github.com/foundriesio/fioconfig.git && cd fioconfig \
#     && make bin/fioconfig-linux-amd64 \
#     && cp ./bin/fioconfig-linux-amd64 /usr/bin/fioconfig

WORKDIR /build
RUN git clone https://github.com/foundriesio/fioctl.git && cd fioctl \
    && git checkout v0.46 \
    && make fioctl-linux-amd64 \
    && cp ./bin/fioctl-linux-amd64 /usr/bin/fioctl

# Build fiopull: PR #22 merged to foundriesio/ostreeuploader master.
WORKDIR /build
RUN git clone https://github.com/foundriesio/ostreeuploader.git && cd ostreeuploader \
    && make fiopull \
    && cp ./bin/fiopull /usr/bin/fiopull


FROM foundries/aklite-dev:ubuntu-26.10 AS aklite

# Install composectl
COPY --from=composeapp /build/composeapp/bin/composectl /usr/bin/

# # Install fioconfig
# COPY --from=composeapp /build/fioconfig/bin/fioconfig-linux-amd64 /usr/bin/fioconfig

# Install fioctl
COPY --from=composeapp /build/fioctl/bin/fioctl-linux-amd64 /usr/bin/fioctl

# Install fiopull at the default FioPullBin path so the size check runs.
COPY --from=composeapp /build/ostreeuploader/bin/fiopull /usr/bin/fiopull

# fiocli uploads Targets and creates rollouts for the update-server backend
# (e2e-test-create-targets-local.py). Same release as fioserver in update-server/Dockerfile.
RUN wget -O /usr/bin/fiocli "https://github.com/foundriesio/update-server/releases/download/v0.9.5/fiocli-linux-amd64" && \
    chmod +x /usr/bin/fiocli


# The base image's apt lists go stale.
RUN apt-get update && apt-get install -y libboost-iostreams-dev python3-pytest iproute2 ca-certificates

# Trust the update-server backend's registry: `composectl pull` requires HTTPS.
COPY registry/certs/registry.crt /usr/local/share/ca-certificates/e2e-registry.crt
RUN update-ca-certificates

# Install lmp-device-register
# A commit on main: no release yet builds against OpenSSL 4.0 with the HSM options. Built without
# REQUIRE_FACTORY, so there's no --factory (DEVICE_FACTORY sets it) and the API URLs are built in.
RUN git clone https://github.com/foundriesio/lmp-device-register \
  && cd lmp-device-register && git checkout d8f39a6 \
  && cmake -S . -B ./build -DDOCKER_COMPOSE_APP=ON -DHARDWARE_ID=intel-corei7-64 -DDISABLE_PKCS11=OFF \
       -DDEVICE_API=https://api.foundries.io/ota/devices/ -DOAUTH_API=https://app.foundries.io/oauth \
  && cmake --build ./build --target install

# Install docker credential helper and auth configuration
COPY config.json /usr/lib/docker/config.json
COPY docker-credential-fio-helper /usr/bin/docker-credential-fio-helper

# Install gosu required for the entry/startup script to add a user and group in the container
RUN wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/1.14/gosu-amd64" && \
    chmod +x /usr/local/bin/gosu && \
    gosu nobody true

# Copy the entrypoint script
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

RUN git config --global --add safe.directory .

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
