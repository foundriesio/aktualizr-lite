# aktualizr-lite

C++ implementation of [TUF](https://theupdateframework.io/) OTA update client based on [aktualizr](https://github.com/advancedtelematic/aktualizr)

## Getting Started

### Dependencies

[List of aktualizr-lite dependencies](https://github.com/advancedtelematic/aktualizr#dependencies).
The [Dockerfile](./docker/Dockerfile) defining a Docker container also includes the dependencies required for building and unit testing aktualizr-lite.

### Build

```
git clone --recursive https://github.com/foundriesio/aktualizr-lite
cd aktualizr-lite
```

If you cloned the repo without `--recursive` flag run:

```
git submodule update --init --recursive
```

Run `make` to build and/or apply `clang-format` or `clang-tidy` utilities.
Use the environment variables to amend the `make` commands behavior.

```
[<MAKE ENV VARS>] make [-f dev-flow.mk] <config | build | format | tidy >
```

The make environment variables are:

*  `BUILD_DIR` — a build directory,`./build` by default;
*  `CCACHE_DIR` — a ccache directory,`./ccache` by default;
*  `TARGET` — a target to build, `aklite-test` by default. Could be `aktualizr-lite` if no need to build tests;
*  `CXX` — a compiler to use, `clang++` or the compiler specified in a host's `env` by default.

If `-f dev-flow.mk` is specified, then the `make` command is executed on a host, otherwise the `foundries/aklite-dev` container is used.
A user can specify their own container to run the commands in by overriding `CONTAINER` environment variable.

#### Build Custom Client Example

[The example of the custom client](./examples/custom-client-cxx/main.cc) depends on the `libaktualizr` and `libaktualizr_lite` libraries,
therefore they should be built prior to building of the example. Then, run:
```
make [-f dev-flow.mk] custom-client
```

### Test

```
[<MAKE ENV VARS>] make [-f dev-flow.mk] test
```

The make environment variables are:

*  `TEST_LABEL` - a label (regexp) of the target tests. For example, `aklite:compose-apps`, by default, is set to `aklite` - run all `aktualizr-lite tests`;
*  `CTEST_ARGS` - additional `ctest` parameters, by default is set to `--output-on-failure`.
*  `GTEST_FILTER` - a regexp to filter tests within gtest suits, e.g. `GTEST_FILTER="*OstreeUpdateNoSpaceRetry*" TEST_LABEL="aklite:liteclient" make`.

### Usage

[Run aktualizr-lite locally against your Factory](./how-to-run-locally.md)

#### Run Custom Client Example

Prior to running of the custom client example, a user should execute all steps listed in [the how to run locally guide](./how-to-run-locally.md).
Then, the following command will start the custom client:
```
LD_LIBRARY_PATH="$LD_LIBRARY_PATH:./build/aktualizr/src/libaktualizr/:./build/src"  \
            AKLITE_CONFIG_DIR=<device config dir | sota.toml> ./build-custom/custom-sota-client
```
or in the dev container:

```
docker run --rm -v $PWD:$PWD -w $PWD \
    -e LD_LIBRARY_PATH="$LD_LIBRARY_PATH:./build-cont/aktualizr/src/libaktualizr/:./build-cont/src" \
    -e AKLITE_CONFIG_DIR="<device config dir | sota.toml>"  foundries/aklite-dev ./build-cont-custom/custom-sota-client
```
