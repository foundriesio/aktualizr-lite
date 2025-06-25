FROM golang:1.22.2-bookworm AS composeapp
# Build composeapp
WORKDIR /build
RUN git clone https://github.com/foundriesio/composeapp.git && cd composeapp \
    && STOREROOT=/var/sota/reset-apps COMPOSEROOT=/var/sota/compose-apps BASESYSTEMCONFIG=/usr/lib/docker make \
    && cp ./bin/composectl /usr/bin/

# We may add fioctl and fioconfig to the test sequence. For now, we don't use them
# WORKDIR /build
# RUN git clone https://github.com/foundriesio/fioconfig.git && cd fioconfig \
#     && make bin/fioconfig-linux-amd64 \
#     && cp ./bin/fioconfig-linux-amd64 /usr/bin/fioconfig

# WORKDIR /build
# RUN git clone https://github.com/foundriesio/fioctl.git && cd fioctl \
#     && make fioctl-linux-amd64 \
#     && cp ./bin/fioctl-linux-amd64 /usr/bin/fioctl


FROM foundries/aklite-dev:ubuntu-25.04 AS aklite

# Install composectl
COPY --from=composeapp /build/composeapp/bin/composectl /usr/bin/

# # Install fioconfig
# COPY --from=composeapp /build/fioconfig/bin/fioconfig-linux-amd64 /usr/bin/fioconfig

# # Install fioctl
# COPY --from=composeapp /build/fioctl/bin/fioctl-linux-amd64 /usr/bin/fioctl


# Install lmp-device-register
RUN apt-get install -y libboost-iostreams-dev

RUN git clone https://github.com/foundriesio/lmp-device-register \
  && cd lmp-device-register && git checkout mp-90 \
  && cmake -S . -B ./build -DDOCKER_COMPOSE_APP=ON -DHARDWARE_ID=intel-corei7-64 && cmake --build ./build --target install

# Install docker credential helper and auth configuration
COPY config.json /usr/lib/docker/config.json
COPY docker-credential-fio-helper /usr/bin/docker-credential-fio-helper

# Install gosu required for the entry/startup script to add a user and group in the container
RUN wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/1.14/gosu-amd64" && \
    chmod +x /usr/local/bin/gosu && \
    gosu nobody true

RUN  apt-get install -y python3-pytest

# Copy the entrypoint script
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
