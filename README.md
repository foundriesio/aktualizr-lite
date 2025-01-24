# Remove me after test
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

### Development And Testing in Containerized Environment

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

#### Running Automated Tests Against FoundriesFactory

The `docker-e2e-test/e2e-test.py` script contains a sequence of aktualizr-lite operations that rely on pre-created Targets.
It verifies several aspects of aktualizr-lite execution, such as return codes, generated events, invoked callbacks, etc,
in both online and offline modes.

The `docker-e2e-test/e2e-test-create-target.py` script creates the targets that match the ones expected by `e2e-test.py` in x86-64 factories.
The script updates the factory's git repositories (`ci-script` and `containers`),
so usage of production factories is not recommended.
Python library requirements are listed in `requirements.txt`,
and it also depends on `fioctl`, `fiopush`, `ostree` and `git` commands.

Execution requires some environment variables must be set:

```bash
export FACTORY=<factory_name>
export TAG=<tag>
export USER_TOKEN=<fio_user_token>

python docker-e2e-test/e2e-test-create-targets.py
```

Once the targets are created, the script outputs instructions on how to run the end-to-end tests,
which are copied bellow.
Notice that before running the end-to-end tests sequence,
the offline bundles have to be fetched in order to allow the test script to verify the offline updates feature.
This requires the setting of an additional `BASE_TARGET_VERSION` environment variable, shown at the end of `e2e-test-create-targets.py` execution.

```bash
# Required environment variables for e2e tests:
export FACTORY=<factory>
export TAG=<tag>
export USER_TOKEN=<fio_user_token>
export BASE_TARGET_VERSION=<version>

# Create offline bundles:
mkdir -p offline-bundles
for version_offset in 0 1 2 3 4 5 6 8 9 10 11; do
version=$[ $version_offset + $BASE_TARGET_VERSION ];
echo $version;
fioctl targets offline-update \
--ostree-repo-source=./e2e-test-targets/small-ostree/repo \
--allow-multiple-targets intel-corei7-64-lmp-$version \
-f ${FACTORY} offline-bundles/unified  \
--tag ${TAG} || break;
done

# Run tests:
./dev-shell-e2e-test.sh pytest docker-e2e-test/e2e-test.py
```

### Usage on Local Environment

[Run aktualizr-lite locally against your Factory](./how-to-run-locally.md)

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
