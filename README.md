# aktualizr-lite


C++ implementation of [TUF](https://theupdateframework.io/) OTA update client based on [aktualizr](https://github.com/advancedtelematic/aktualizr)


## Getting Started

### Dependencies
List of aktualizr-lite dependencies can be found [here](https://github.com/advancedtelematic/aktualizr#dependencies)

#### libostree
libostree is one of the key dependencies that stands out from the others.
It's worth noting that aktualizr-lite as well as aktualizr requires libostree built with libcurl support which is not so in the case of default libostree installation for Ubuntu systems.
The following are instructions on building of libostree with libcurl support
```
mkdir ostree && cd ostree
git init && git remote add origin https://github.com/ostreedev/ostree
git fetch origin v2020.7 && git checkout FETCH_HEAD

./autogen.sh CFLAGS='-Wno-error=missing-prototypes' --with-libarchive --disable-man --with-builtin-grub2-mkconfig --with-curl --without-soup --prefix=/usr
sudo make -j6 install
```

### Build

```
git clone --recursive https://github.com/foundriesio/aktualizr-lite
cd aktualizr-lite
```
or if you cloned the repo without `--recursive` flag
```
git submodule update --init --recursive
```
Initialize a build directory
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```
if you prefer Ninja backend
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -GNinja
```
turn on clang-tidy
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -GNinja  -DCLANG_TIDY=ON
```
turn on PKCS#11 compatible HSM support (store mTLS private key and cert in HSM)
```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -GNinja  -DCLANG_TIDY=ON -DBUILD_P11=ON
```

Build aktualizr-lite
```
cmake --build build --target aklite -- -j6
or if Ninja is used just
cmake --build build --target aklite
```

Build tests
```
cmake --build build --target aklite-tests -- -j6
or if Ninja is used just
cmake --build build --target aklite-tests
```

### Test
```
cd build
ctest -L aklite
```

#### Build and test in a docker container
```
./unit-test
```

### Usage
[Run aktualizr-lite locally against your Foundries Factory](./how-to-run-locally.md)
