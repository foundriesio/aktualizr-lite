# aktualizr-lite


C++ implementation of [TUF](https://theupdateframework.io/) OTA update client based on [aktualizr](https://github.com/advancedtelematic/aktualizr)


## Getting Started

### Dependencies
List of aktualizr-lite dependencies can be found [here](https://github.com/advancedtelematic/aktualizr#dependencies)

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

#### Configure
TODO

#### Run
```
./build/src/aktualizr-lite -h
```
