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

### Unit Test

```
[<MAKE ENV VARS>] make [-f dev-flow.mk] test
```

The make environment variables are:

*  `TEST_LABEL` - a label (regexp) of the target tests. For example, `aklite:compose-apps`, by default, is set to `aklite` - run all `aktualizr-lite tests`;
*  `CTEST_ARGS` - additional `ctest` parameters, by default is set to `--output-on-failure`.
*  `GTEST_FILTER` - a regexp to filter tests within gtest suits, e.g. `GTEST_FILTER="*OstreeUpdateNoSpaceRetry*" TEST_LABEL="aklite:liteclient" make`.


## Documentation

### Development And Testing in Containerized Environment

[Development And Testing in Containerized Environment](./docs/development-container.md)

### Usage on Local Environment

[Run aktualizr-lite locally against your Factory](./docs/how-to-run-locally.md)

### Custom Client Example

Aktualizr-lite provides an API that can be used for creating a custom update client.
An example of such client is available at https://github.com/foundriesio/sotactl

### Testing Changes on Device

The easiest way to build an `aktualizr-lite` executable that is compatible with the LmP environment used in a device
is to use `bitbake` in conjunction with the Yocto layers from the device's Factory.
In order to use a local source tree during build, a custom `bitbake` workflow can be employed, as described in
https://foundries.io/insights/blog/yocto-hack-for-everyone/.

When dealing with hardware independent logic,
the recommended way of testing is using the [containerized development environment](#development-and-testing-in-containerized-environment).
Alternatively, a Qemu environment can also be used.
The specific instructions on use of Qemu as a FoundriesFactory device varies according to the architecture in use:
- [X86-64](https://docs.foundries.io/94/user-guide/qemu/x86_64.html)
- [ARM64](https://docs.foundries.io/94/user-guide/qemu/arm64.html)
- [ARM](https://docs.foundries.io/94/user-guide/qemu/arm.html)

### Command Line Interface

[Command Line Interface](./docs/command-line-interface.md)

### Configuration

[Configuration](./docs/configuration.md)

### Architecture Overview

[Architecture Overview](./docs/architecture.md)

### Callbacks

[Callbacks](./docs/callbacks.md)
