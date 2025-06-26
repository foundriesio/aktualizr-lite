# aktualizr-lite

C++ implementation of [TUF](https://theupdateframework.io/) OTA update client based on [aktualizr](https://github.com/advancedtelematic/aktualizr)


## Reference Manual

### Customizing Over-the-air Update Flow

By default, Secure Over The Air (SOTA) update operates as a daemon process (Aktualizr-Lite) which periodically checks for updates. If an update is available, it will automatically download, and install it to a device that is following the update tag.

This is not always the desired operation. There are a couple ways to control this operation:

* [Command Line Interface - CLI](./docs/command-line-interface.md)
* [Callbacks](./docs/callbacks.md)
* [Custom Update Agent](./docs/custom-client.md)


### Configuration

[Configuration](./docs/configuration.md)


### Architecture Overview

[Architecture Overview](./docs/architecture.md)


## Development and Testing


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

### Unit Test

```
[<MAKE ENV VARS>] make [-f dev-flow.mk] test
```

The make environment variables are:

*  `TEST_LABEL` - a label (regexp) of the target tests. For example, `aklite:compose-apps`, by default, is set to `aklite` - run all `aktualizr-lite tests`;
*  `CTEST_ARGS` - additional `ctest` parameters, by default is set to `--output-on-failure`.
*  `GTEST_FILTER` - a regexp to filter tests within gtest suits, e.g. `GTEST_FILTER="*OstreeUpdateNoSpaceRetry*" TEST_LABEL="aklite:liteclient" make`.


### Running and Testing Aktualizr-lite 

Aktualizr-lite is installed by default in devices running the LmP distribution.
In order to test features, and specially verifying changes in the `aktualizr-lite` code,
using the containerized enviromnent is more appropriate.

Here are instructions on running aktualizr-lite in different environments:
* [Running on Containerized Environment](./docs/development-container.md)
* [Testing Changes on Device](./docs/testing-changes-on-device.md)
* [Run aktualizr-lite locally against your Factory](./docs/how-to-run-locally.md)
