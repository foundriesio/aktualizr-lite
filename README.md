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

### Development Test Environment

aktualizr-lite can be executed inside a development container that contains all the required library and tools.
It works in conjunction with a container running dockerd,
providing an environment where most of the functional aspects can be tested.
It allows the communication with an actual factory instance, getting updates and sending back events.

#### Prerequisites
1. Linux host/VM (tested on Ubuntu and Arch Linux)
2. Docker engine and client, version >= 20.10.xx
3. Docker compose, version >= 2.22.0

#### Set required environment variables
1. `FACTORY` - Name of your FoundriesFactory.
2. `USER_TOKEN` - Token to access your FoundriesFactory obtained from [Foundries.io settings page](https://app.foundries.io/settings/tokens/).

#### Development in the development container
Run `./dev-shell-e2e-test.sh`. The initial/first run may take some time as necessary container images are downloaded and built. Subsequent runs will be faster.

##### Register/Unregister device
Inside the development container, run `make register` or `make unregister` to register or unregister a device, respectively.
Override the `DEVICE_TAG` environment variable if you need to register a device and set its tag to a non-default value (`main`).
For example, `DEVICE_TAG=devel make register`.

##### Build aktualizr-lite
Run `make -f dev-flow.mk` to build the aktualizr-lite client.

##### Configure your development device
You can add additional configuration `*.toml` snippets to the default device configuration by placing them in `<sota-project-home-dir>/.device/etc.sota`.

For example, a subset of applications that should to be started automatically can be set by creating a `<sota-project-home-dir>/.device/etc.sota/z-90-apps.toml` file with the following content:
```
[pacman]
compose_apps="myapp1,myapp2"
```

##### Check/Update local TUF metadata
Run `make check` to update the local TUF (The Update Framework) repository and get a list of available targets. Alternatively, run `./build/src/atualizr-lite check`.

##### Update your development device
1. Run `make update` to update your development device to the latest available target. The initial update may take some time since it pulls the target's ostree repository from scratch. Subsequent updates are faster as they pull only the differences between two commits.
2. If a device reboot is required, run `make reboot`.
3. Finalize the update and run updated apps, if any, by running `make run`.

##### Develop and debug aktualizr-lite
After the initial update, you can continue developing and debugging aktualizr-lite:
1. Make code changes.
2. Build the code with `make -f dev-flow.mk`.
3. Run or debug one of the SOTA client commands, for example:
   ```bash
   ./build/src/atualizr-lite check               # Check for TUF metadata changes and update local TUF repository if any changes are found
   ./build/src/atualizr-lite pull [<target>]     # Pull the specified target content (ostree and/or apps)
   ./build/src/atualizr-lite install [<target>]  # Install the pulled target
   ./build/src/atualizr-lite run                 # Finalize target installation if required after a device reboot

   gdb --args ./build/src/atualizr-lite [<command&params>]   # Debug

###### Emulating device reboot
To emulate a device reboot in the development container, remove the `/var/run/aktualizr-session/need_reboot` file if it exists.

### Usage on Local Environment

[Run aktualizr-lite locally against your Factory](./how-to-run-locally.md)

### Custom Client Example

Aktualizr-lite provides an API that can be used for creating a custom update client.
An example of such client is available at https://github.com/foundriesio/sotactl
