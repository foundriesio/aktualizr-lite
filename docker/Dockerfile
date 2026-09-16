FROM golang:1.24.13-bookworm AS composeapp
# Build composeapp
WORKDIR /build
RUN git clone https://github.com/foundriesio/composeapp.git && \
    cd composeapp && git checkout lmp-97.0.0 && make && cp ./bin/composectl /usr/bin/


FROM ubuntu:stonking
LABEL Description="Aktualizr testing dockerfile for Ubuntu Focal with static checks"

ENV DEBIAN_FRONTEND noninteractive

# It is important to run these in the same RUN command, because otherwise
# Docker layer caching breaks us
RUN apt-get update && apt-get -y install --no-install-suggests --no-install-recommends \
  apt-transport-https \
  apt-utils \
  asn1c \
  autoconf \
  automake \
  bison \
  bash \
  ccache \
  clang \
  clang-format \
  clang-tidy \
  clang-tools \
  cmake \
  curl \
  doxygen \
  e2fslibs-dev \
  g++ \
  gcc \
  gdb \
  git \
  vim \
  graphviz \
  jq \
  lcov \
  libarchive-dev \
  libyaml-dev \
  libboost-dev \
  libboost-log-dev \
  libboost-program-options-dev \
  libboost-test-dev \
  libboost-thread-dev \
  libcurl4-openssl-dev \
  pkcs11-provider \
  libicu-dev \
  libexpat1-dev \
  libfuse-dev \
  libglib2.0-dev \
  libgpgme11-dev \
  libgtest-dev \
  liblzma-dev \
  libp11-dev \
  libsofthsm2 \
  libsofthsm2-dev \
  libsodium-dev \
  libsqlite3-dev \
  libssl-dev \
  libltdl-dev \
  libtool \
  lshw \
  make \
  net-tools \
  ninja-build \
  opensc \
  pkg-config \
  psmisc \
  python-is-python3 \
  python3-dev \
  python3-gi \
  python3-openssl \
  python3-pip \
  python3-venv \
  softhsm2 \
  sqlite3 \
  strace \
  valgrind \
  wget \
  xsltproc \
  zip \
  sudo \
  shellcheck \
  gcovr \
  libfyaml-dev \
  libfyaml-utils \
  python3-docker \
  skopeo

# OSTREE with libcurl support for HSM
#  This allows ostree calls to libcurl to be able to keys controlled by pkcs11
WORKDIR /tmp
RUN mkdir ostree && cd ostree && git init && git remote add origin https://github.com/ostreedev/ostree && \
    git fetch origin v2024.10 && git checkout FETCH_HEAD && \
    ./autogen.sh CC="clang" CFLAGS='-Wno-error=missing-prototypes' \
    --with-libarchive \
    --disable-man \
    --with-builtin-grub2-mkconfig \
    --with-curl --without-soup --prefix=/usr && \
    make -j8  install

# Add Docker's official GPG key:
RUN apt-get update && apt-get install -y ca-certificates curl
RUN install -m 0755 -d /etc/apt/keyrings
RUN curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
RUN chmod a+r /etc/apt/keyrings/docker.asc

# Add the repository to Apt sources. Docker's own apt repo lags behind Ubuntu's release
# cadence and has no "stonking" component yet, so fall back to the latest codename it does
# support (resolute, 26.04) - docker-ce-cli/docker-compose-plugin are Go binaries with no
# meaningful ABI dependency on the host's exact Ubuntu release.
RUN echo \
  "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu \
  resolute stable" | \
  tee /etc/apt/sources.list.d/docker.list > /dev/null

RUN apt-get update && apt-get install -y docker-ce-cli docker-compose-plugin

# Restore a standalone "docker-compose" command for tests/tooling that still invoke it directly
# instead of "docker compose" - the docker-compose-plugin binary works fine either way.
RUN ln -sf /usr/libexec/docker/cli-plugins/docker-compose /usr/local/bin/docker-compose

# Install composeapp
COPY --from=composeapp /build/composeapp/bin/composectl /usr/bin/

WORKDIR /
