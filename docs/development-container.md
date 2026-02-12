# Development And Testing in a Containerized Environment

Aktualizr-lite can be executed inside a development container containing the required libraries and tools.
It works in conjunction with a container running dockerd,
providing an environment where most of the functional aspects can be tested.
It allows the communication with an actual Factory instance,
getting updates and sending back events.

## Prerequisites

1. Linux host/VM (tested on Ubuntu and Arch Linux)
2. Docker engine and client, version >= 20.10.xx
3. Docker compose, version >= 2.22.0

## Set Required Environment Variables

1. `FACTORY` - Name of your Factory.
2. `USER_TOKEN` - Token to access your Factory obtained from [Foundries.io settings page](https://app.foundries.io/settings/tokens/).

## Development in the Development Container

Run `./dev-shell-e2e-test.sh`.
The initial/first run may take some time as necessary container images are downloaded and built.
Subsequent runs will be faster.

### Register/Unregister Device

Inside the development container,
run `make register` or `make unregister` to register or unregister a device, respectively.
Override the `DEVICE_TAG` environment variable if you need to register a device and set its tag to a non-default value (`main`).
For example, `DEVICE_TAG=devel make register`.

### Build Aktualizr-Lite

Run `make -f dev-flow.mk` to build the aktualizr-lite client.

### Configure Your Development Device

You can add additional`*.toml`configuration snippets to the default device config by placing them in `<sota-project-home-dir>/.device/etc.sota`.

For example, to set a subset of applications that should start automatically,
create a `<sota-project-home-dir>/.device/etc.sota/z-90-apps.toml` file with the following content:

```
[pacman]
compose_apps="myapp1,myapp2"
```

### Check/Update Local TUF Metadata

Run `make check` to update the local TUF (The Update Framework) repository and get a list of available targets.
Alternatively, run `./build/src/atualizr-lite check`.

### Update Your Development Device

1. Run `make update` to update your development device to the latest available Target.
   The initial update may take some time since it pulls the Target's ostree repository from scratch.
   Subsequent updates are faster as they pull only the differences between two commits.
2. If a device reboot is required, run `make reboot`.
3. Finalize the update and run updated apps, if any, by running `make run`.

### Develop and Debug Aktualizr-Lite

After the initial update, you can continue developing and debugging aktualizr-lite:

1. Make code changes
2. Build the code with `make -f dev-flow.mk`
3. Run or debug one of the SOTA client commands, for example:

   ```bash
   ./build/src/atualizr-lite check               # Check for TUF metadata changes and update local TUF repository if any changes are found
   ./build/src/atualizr-lite pull [<target>]     # Pull the specified target content (ostree and/or apps)
   ./build/src/atualizr-lite install [<target>]  # Install the pulled target
   ./build/src/atualizr-lite run                 # Finalize target installation if required after a device reboot
   gdb --args ./build/src/atualizr-lite [<command&params>]   # Debug
   ```

#### Emulating Device Reboot

To emulate a device reboot in the development container,
remove the `/var/run/aktualizr-session/need_reboot` file if it exists.

## Running Automated Tests Against FoundriesFactory

The `docker-e2e-test/e2e-test.py` script contains a sequence of aktualizr-lite operations that rely on pre-created Targets.
It verifies several aspects of aktualizr-lite execution,
such as return codes, generated events, invoked callbacks, etc,
in both online and offline modes.

The `docker-e2e-test/e2e-test-create-target.py` script creates the Targets that match the ones expected by `e2e-test.py` in x86-64 factories.
The script updates the Factory's git repositories (`ci-script` and `containers`),
so usage in production is not recommended.
Python library requirements are listed in `requirements.txt`,
and also depends on `fioctl`, `fiopush`, `ostree` and `git` commands.

Execution requires some environment variables must be set:

```bash
export FACTORY=<factory_name>
export TAG=<tag>
export USER_TOKEN=<fio_user_token>

python docker-e2e-test/e2e-test-create-targets.py
```

Once the Targets are created,
the script outputs instructions on how to run the end-to-end tests,
which are copied below.
Notice that before running the end-to-end test sequence,
the offline bundles have to be fetched,
allowing the test script to verify the offline updates feature.
This requires the setting of an additional `BASE_TARGET_VERSION` environment variable,
shown at the end of `e2e-test-create-targets.py` execution.

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

