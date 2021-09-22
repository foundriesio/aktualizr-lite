FROM ubuntu:focal
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
  clang-10 \
  clang-format-10 \
  clang-tidy-10 \
  clang-tools-10 \
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
  zip

RUN ln -s clang-10 /usr/bin/clang && \
    ln -s clang++-10 /usr/bin/clang++

WORKDIR /tmp
RUN git clone https://github.com/pantoniou/libfyaml.git
RUN cd libfyaml && ./bootstrap.sh && ./configure --prefix /usr && make && make install
RUN ldconfig

# OSTREE with libcurl support for HSM
#  This allows ostree calls to libcurl to be able to keys controlled by pkcs11
WORKDIR /tmp
RUN mkdir ostree && cd ostree && git init && git remote add origin https://github.com/ostreedev/ostree && \
    git fetch origin v2020.7 && git checkout FETCH_HEAD && \
    ./autogen.sh CFLAGS='-Wno-error=missing-prototypes' \
    --with-libarchive \
    --disable-man \
    --with-builtin-grub2-mkconfig \
    --with-curl --without-soup --prefix=/usr && \
    make -j8  install

# Docker in docker
RUN curl -sSL https://get.docker.com/ | sh

# Install the magic wrapper.
ADD ./docker/wrapdocker /usr/local/bin/wrapdocker
RUN chmod +x /usr/local/bin/wrapdocker

# Install skopeo
RUN echo 'deb http://download.opensuse.org/repositories/devel:/kubic:/libcontainers:/stable/xUbuntu_20.04/ /' > /etc/apt/sources.list.d/devel:kubic:libcontainers:stable.list && \
    wget -nv https://download.opensuse.org/repositories/devel:kubic:libcontainers:stable/xUbuntu_20.04/Release.key -O- | apt-key add -
RUN apt-get update && apt-get -y install skopeo && ln -s /usr/bin/skopeo /sbin/skopeo

RUN useradd testuser

WORKDIR /
