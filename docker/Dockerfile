FROM golang:1.22.2-bookworm AS composeapp
# Build composeapp
WORKDIR /build
RUN git clone https://github.com/foundriesio/composeapp.git && \
    cd composeapp && git checkout 1b6f1f2d8f55d40fef10c8ddfef224a9a58caa27 && make && cp ./bin/composectl /usr/bin/


FROM ubuntu:jammy
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
  clang-11 \
  clang-format-11 \
  clang-tidy-11 \
  clang-tools-11 \
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
  libboost-system-dev \
  libboost-test-dev \
  libboost-thread-dev \
  libcurl4-openssl-dev \
  libengine-pkcs11-openssl \
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
  docker-compose \
  sudo \
  shellcheck

RUN ln -s clang-11 /usr/bin/clang && \
    ln -s clang++-11 /usr/bin/clang++

WORKDIR /tmp
RUN git clone https://github.com/pantoniou/libfyaml.git
RUN cd libfyaml && ./bootstrap.sh && ./configure --prefix /usr && make && make install
RUN ldconfig

# OSTREE with libcurl support for HSM
#  This allows ostree calls to libcurl to be able to keys controlled by pkcs11
WORKDIR /tmp
RUN mkdir ostree && cd ostree && git init && git remote add origin https://github.com/ostreedev/ostree && \
    git fetch origin v2021.6 && git checkout FETCH_HEAD && \
    ./autogen.sh CC="clang" CFLAGS='-Wno-error=missing-prototypes' \
    --with-libarchive \
    --disable-man \
    --with-builtin-grub2-mkconfig \
    --with-curl --without-soup --prefix=/usr && \
    make -j8  install

# Install composeapp
COPY --from=composeapp /build/composeapp/bin/composectl /usr/bin/
# Install skopeo
RUN echo 'deb http://download.opensuse.org/repositories/devel:/kubic:/libcontainers:/stable/xUbuntu_20.04/ /' > /etc/apt/sources.list.d/devel:kubic:libcontainers:stable.list && \
    wget -nv https://download.opensuse.org/repositories/devel:kubic:libcontainers:stable/xUbuntu_20.04/Release.key -O- | apt-key add -
RUN apt-get update && apt-get -y install skopeo && ln -s /usr/bin/skopeo /sbin/skopeo

# Install specific version of docker-compose
RUN pip3 install docker==4.2.1 && pip3 install docker-compose==1.26 pyOpenSSL==23.0.0

WORKDIR /
