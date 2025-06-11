FROM golang:1.22.2-bookworm AS composeapp
# Build composeapp
WORKDIR /build
RUN git clone https://github.com/foundriesio/composeapp.git && cd composeapp \
    && STOREROOT=/var/sota/reset-apps COMPOSEROOT=/var/sota/compose-apps BASESYSTEMCONFIG=/usr/lib/docker make 

WORKDIR /build_lmp95
RUN git clone https://github.com/foundriesio/composeapp.git && cd composeapp \
    && git checkout mp-95 \
    && STOREROOT=/var/sota/reset-apps COMPOSEROOT=/var/sota/compose-apps BASESYSTEMCONFIG=/usr/lib/docker make 

# We may add fioctl and fioconfig to the test sequence. For now, we don't use them
# WORKDIR /build
# RUN git clone https://github.com/foundriesio/fioconfig.git && cd fioconfig \
#     && make bin/fioconfig-linux-amd64 \
#     && cp ./bin/fioconfig-linux-amd64 /usr/bin/fioconfig

# WORKDIR /build
# RUN git clone https://github.com/foundriesio/fioctl.git && cd fioctl \
#     && make fioctl-linux-amd64 \
#     && cp ./bin/fioctl-linux-amd64 /usr/bin/fioctl


FROM foundries/aklite-dev AS aklite

WORKDIR /build_lmp95
RUN git clone --recurse-submodules https://github.com/foundriesio/aktualizr-lite.git && cd aktualizr-lite \
    && git checkout --recurse-submodules mp-95 \
    && make -f dev-flow.mk \
    && cp ./build/src/aktualizr-lite /usr/bin/aktualizr-lite_lmp95

  
# Install composectl
COPY --from=composeapp /build/composeapp/bin/composectl /usr/bin/composectl_main
COPY --from=composeapp /build_lmp95/composeapp/bin/composectl /usr/bin/composectl_lmp95
RUN rm -f /usr/bin/composectl

RUN chmod a+w /usr/bin/

# # Install fioconfig
# COPY --from=composeapp /build/fioconfig/bin/fioconfig-linux-amd64 /usr/bin/fioconfig

# # Install fioctl
# COPY --from=composeapp /build/fioctl/bin/fioctl-linux-amd64 /usr/bin/fioctl


# Install lmp-device-register
RUN  apt-get install -y libboost-iostreams-dev

RUN git clone https://github.com/foundriesio/lmp-device-register \
  && cd lmp-device-register && git checkout mp-90 \
  && cmake -S . -B ./build -DDOCKER_COMPOSE_APP=ON -DHARDWARE_ID=intel-corei7-64 && cmake --build ./build --target install


# Add Docker's official GPG key:
RUN apt-get update && apt-get install -y ca-certificates curl
RUN install -m 0755 -d /etc/apt/keyrings
RUN curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
RUN chmod a+r /etc/apt/keyrings/docker.asc

# Add the repository to Apt sources:
RUN echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu \
  $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | \
  tee /etc/apt/sources.list.d/docker.list > /dev/null

RUN apt-get update && apt-get install -y docker-ce-cli docker-compose-plugin

# Install docker credential helper and auth configuration
COPY config.json /usr/lib/docker/config.json
COPY docker-credential-fio-helper /usr/bin/docker-credential-fio-helper

# Install gosu required for the entry/startup script to add a user and group in the container
RUN wget -O /usr/local/bin/gosu "https://github.com/tianon/gosu/releases/download/1.14/gosu-amd64" && \
    chmod +x /usr/local/bin/gosu && \
    gosu nobody true

# Install pytest
RUN pip install --no-cache-dir --upgrade pip && \
    pip install --no-cache-dir pytest

    # Copy the entrypoint script
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# Set entrypoint
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
