# aktualizr-lite


C++ implementation of [TUF](https://theupdateframework.io/) OTA update client based on [aktualizr](https://github.com/advancedtelematic/aktualizr)


## Getting Started

### Dependencies
List of aktualizr-lite dependencies can be found [here](https://github.com/advancedtelematic/aktualizr#dependencies) or in the [Dockerfile](./docker/Dockerfile) that defines a docker container which includes the all dependencies required for aktualizr-lite building and unit testing.

### Build

```
git clone --recursive https://github.com/foundriesio/aktualizr-lite
cd aktualizr-lite
```
or if you cloned the repo without `--recursive` flag
```
git submodule update --init --recursive
```
Run `make` to build and/or apply `clang-format` or `clang-tidy` utilities. Use the environment variables to amend the `make` commands behavior.
```
[<MAKE ENV VARS>] make [-f dev-flow.mk] <config | build | format | tidy >

```
The make environment variables are:
*  `BUILD_DIR` - a build directory, `./build` by default;
*  `CCACHE_DIR` - a ccache directory, `./ccache` by default;
*  `TARGET` - a target to build, `aklite-test` by default. Could be `aktualizr-lite` if no need to build tests;
*  `CXX` - a compiler to use, `clang++` or the compiler specified in a host's `env` by default.

If `-f dev-flow.mk` is specified then the `make` command is executed on a host, otherwise - in the `foundries/aklite-dev` container.
A user can specify their own container to run the commands in by overriding `CONTAINER` environment variable.

### Test
```
[<MAKE ENV VARS>] make [-f dev-flow.mk] test
```

The make environment variables are:
*  `TEST_LABEL` - a label (regexp) of the target tests. For example, `aklite:compose-apps`, by default, is set to `aklite` - run all `aktualizr-lite tests`;
*  `CTEST_ARGS` - additional `ctest` parameters, by default is set to `--output-on-failure`.
*  `GTEST_FILTER` - a regexp to filter tests within gtest suits, e.g. `GTEST_FILTER="*OstreeUpdateNoSpaceRetry*" TEST_LABEL="aklite:liteclient" make`.


### Usage
[Run aktualizr-lite locally against your Foundries Factory](./how-to-run-locally.md)
