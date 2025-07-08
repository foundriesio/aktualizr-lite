# Command Line Interface

The `aktualizr-lite` executable can be invoked to perform individual operations allowing more control over the update flow.

> ℹ️
In order to use the run individual `aktualizr-lite` commands,
the ``aktualizr-lite`` service needs to be stopped with `sudo systemctl stop aktualizr-lite`
and/or disabled with `sudo systemctl disable aktualizr-lite`.

> ℹ️
If `lmp-device-register` is used,
Using ``--start-daemon 0`` is recommended
in order to avoid starting the aktualizr-lite daemon automatically.

```

$ aktualizr-lite --help
Usage:
aktualizr-lite [command] [flags]

Commands:
  daemon      Start the update agent daemon
  update      Update TUF metadata, download and install the selected target
  pull        Download the selected target data to the device, to allow a install operation to be performed
  install     Install a previously pulled target
  list        List the available targets, using current TUF metadata information. No TUF update is performed
  check       Update the device TUF metadata, and list the available targets
  status      Show information of the target currently running on the device
  finalize    Finalize installation by starting the updated apps
  run         Alias for the finalize command
  rollback    Rollback to the previous successfully installed target [experimental]

Flags:
  -h [ --help ]         Print usage
  -v [ --version ]      Prints current aktualizr-lite version
  -c [ --config ] arg   Configuration file or directory path
  --loglevel arg        Set log level 0-5 (trace, debug, info, warning, error,
                        fatal)
  --update-name arg     Name or version of the target to be used in pull,
                        install, and update commands. default=latest
  --install-mode arg    Optional install mode. Supported modes:
                        [delay-app-install]. By default both ostree and apps
                        are installed before reboot
  --interval arg        Override uptane.polling_secs interval to poll for
                        updates when in daemon mode
  --json arg            Output targets information as json when running check
                        and list commands
  --src-dir arg         Directory that contains an offline update bundle.
                        Enables offline mode for check, pull, install, and
                        update commands
  --command arg         Command to be executed
```

> ℹ️ You can find a containerized environment
    for a custom sota client development in
    https://github.com/foundriesio/sotactl?tab=readme-ov-file#development-in-the-development-container.


## Available commands for Command Line Interface (CLI)


### update

The ``update`` command pulls and installs the latest available update to the device,
after updating the TUF metadata.
This includes both OSTree and Docker app Targets:

`sudo aktualizr-lite update`

To update to a specific build number or Target name,
the ``--update-name`` option can be used:

`sudo aktualizr-lite update --update-name <build_number_or_name>`

> ⚠️
Downgrading to a older Target is neither recommended or supported by our team;
doing so may lead to unverified corner cases.
Only choose to do so mindfully.
For any update, always test before rolling out to production devices.

> ℹ️
Since LmP v95, aktualizr-lite will not automatically do a downgrade
when all available targets have a version lower than the current one.
This applies to the aktualizr-lite ``daemon``
as long as ``update``, ``install``, and ``pull`` commands
if no Target name or version is explicitly specified.
In order to allow an automatic downgrade to occur in such situations,
the ``auto-downgrade`` package option has to be set in the aktualizr recipe:
>```
>$ cat meta-subscriber-overrides.git/recipes-sota/aktualizr/aktualizr_%.bbappend
>PACKAGECONFIG:append = " auto-downgrade"
>```

When the OSTree image was changed,
a reboot command is required after installing the update,
followed by the execution on the  ``run`` command to finalize the update process.
The exit code can be used to identify if such reboot is or not required.

The command line interface also allows the update steps to be performed individually,
by calling the ``check``, ``pull`` and ``install`` commands individually.
This allows for a higher level of control over the update process.

**Exit Codes**

- *0*: Success
   - Installation successful. No reboot needed
- *100*: Success
   - Installation succeeded. Reboot to finalize
- *5*: Success
   - Installation succeeded. Reboot to finalize bootloader installation
- *4*: Failure
   - Failure to handle TUF metadata: Check logs for more information
- *6*: Failure
   - There is no Target in the device TUF repo that matches a device tag and/or hardware ID
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *11*: Failure
   - Failed to update TUF metadata: TUF metadata is invalid
- *12*: Failure
   - Failed to update TUF metadata: TUF metadata is expired
- *13*: Failure
   - Failed to update TUF metadata: Download error
- *14*: Failure
   - Failed to update TUF metadata: TUF metadata not found in the provided path (offline mode only)
- *15*: Failure
   - The bundle metadata is invalid (offline mode only).There are a few reasons why the metadata might be invalid:
       1. One or more bundle signatures is/are invalid
       2. The bundle Target's type, whether CI or production, differs from the device`s type
       3. The bundle Target's tag differs from the device`s tag
       4. The offline bundle has expired (its TUF meta has expired)
- *20*: Failure
   - There is no Target that matches the specified name or version
- *21*: Failure
   - Unable to find Target to rollback to after a failure to start Apps at boot on a new version of sysroot
- *30*: Failure
   - Unable to perform operation: there is an installation that needs completion
- *50*: Failure
   - Unable to download Target
- *60*: Failure
   - There is not enough free space to download the Target
- *70*: Failure
   - The pulled Target content is invalid: App compose file is invalid
- *110*: Failure
   - Installation failed, rollback done successfully
- *120*: Failure
   - Installation failed, rollback initiated but requires reboot to finalize
- *130*: Failure
   - Installation failed and rollback operation was not successful
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

### run

Finalize the installation or rollback when a reboot was required,
starting the Target applications.
It is possible that an error is detected at this stage,
which may lead to a rollback being initiated.

#### Exit Codes

- *0*: Success
   - Installation / rollback finalized successfully
- *110*: Failure
   - Finalization failed. A rollback was performed successfully
- *120*: Failure
   - Finalization failed. A rollback was started but requires a reboot to finalize
- *130*: Failure
   - Finalization failed. A rollback was attempted and failed
- *40*: Failure
   - There is no pending installation to be finished
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

### check

The ``check`` command will refresh the Targets metadata from the OTA server,
and present a list of available Targets::

`sudo aktualizr-lite check`

It can used in conjunction with the ``--json 1`` option,
which will change the output format to JSON,
and will, by default, omit other log outputs.

#### JSON output schema
```json
{
  "type": "array",
  "description": "List of available TUF targets",
  "items": [
    {
      "type": "object",
      "properties": {
        "apps": {
          "type": "array",
          "description": "List of the target's applications. If there are no applications, field can be missing, null or []",
          "items": [
            {
              "type": "object",
              "properties": {
                "name": {
                  "type": "string",
                  "description": "Name of the application"
                },
                "on": {
                  "type": "boolean",
                  "description": "True if the application execution is currently enabled in the device"
                },
                "uri": {
                  "type": "string",
                  "description": "Application URI"
                }
              },
              "required": [
                "name",
                "on",
                "uri"
              ]
            }
          ]
        },
        "current": {
          "type": "boolean",
          "description": "True for the target that is currently running on the device"
        },
        "failed": {
          "type": "boolean",
          "description": "True if the target installation was attempted, but failed, leading to a rollback"
        },
        "name": {
          "type": "string",
          "description": "Name of the target"
        },
        "newer": {
          "type": "boolean",
          "description": "True if the target version is higher than the target that is currently running on the device"
        },
        "reason": {
          "type": "string",
          "description": "This field is set only if 'selected' is true. It contains the reason why the target was selected for installation: version upgrade or applications synchronization"
        },
        "selected": {
          "type": "boolean",
          "description": "True if the target is selected for installation based on the default decision logic within aktualizr-lite"
        },
        "version": {
          "type": "integer",
          "description": "Target's version number"
        }
      },
      "required": [
        "name"
      ]
    }
  ]
}
```


#### Exit Codes

- *0*: Success
   - TUF is up to date. No Target update required
- *3*: Success
   - Unable to update TUF metadata, using cached metadata
- *16*: Success
   - Update is required -- new Target version available
- *17*: Success
   - Update is required -- apps need synchronization
- *4*: Failure
   - Failure to handle TUF metadata: Check logs for more information
- *6*: Failure
   - There is no Target in the device TUF repo that matches a device tag and/or hardware ID
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *11*: Failure
   - Failed to update TUF metadata: TUF metadata is invalid
- *12*: Failure
   - Failed to update TUF metadata: TUF metadata is expired
- *13*: Failure
   - Failed to update TUF metadata: Download error
- *14*: Failure
   - Failed to update TUF metadata: TUF metadata not found in the provided path (offline mode only)
- *15*: Failure
   - The bundle metadata is invalid (offline mode only).There are a few reasons why the metadata might be invalid:
       1. One or more bundle signatures is/are invalid
       2. The bundle Target's type, whether CI or production, differs from the device's type
       3. The bundle Target's tag differs from the device`s tag
       4. The offline bundle has expired (its TUF meta has expired)
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

### list

The ``list`` command works in a similar way as ``check``,
presenting the same type of output,
but will **not** refresh the Targets metadata from the OTA server::

   sudo aktualizr-lite list

This command also allows the use of the ``--json 1`` option.

#### Exit Codes

- *3*: Success
   - Cached TUF metadata is valid. No Target update is required
- *16*: Success
   - Update is required -- new Target version available
- *17*: Success
   - Update is required -- apps need synchronization
- *4*: Failure
   - Failure to handle TUF metadata: Check logs for more information
- *6*: Failure
   - There is no Target in the device TUF repo that matches a device tag and/or hardware ID
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

### pull

Download the update data to the device,
allowing a ``install`` operation to be called next.

#### Exit Codes

- *0*: Success
   - Target successfully downloaded
- *4*: Failure
   - Failure to handle TUF metadata: Check logs for more information
- *6*: Failure
   - There is no Target in the device TUF repo that matches a device tag and/or hardware ID
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *20*: Failure
   - There is no Target that matches the specified name or version
- *21*: Failure
   - Unable to find Target to rollback to after a failure to start Apps at boot on a new version of sysroot
- *30*: Failure
   - Unable to perform operation: there is an installation that needs completion
- *50*: Failure
   - Unable to download Target
- *60*: Failure
   - There is not enough free space to download the Target
- *70*: Failure
   - The pulled Target content is invalid: App compose file is invalid
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

### install

Install a previously pulled Target.

A reboot and finalization using the ``run`` is required when the OSTree image was changed.
This is indicated by the command exit code.

#### Exit Codes

- *0*: Success
   - Installation successful. No reboot needed
- *100*: Success
   - Installation succeeded. Reboot to finalize
- *5*: Success
   - Installation succeeded. Reboot to finalize bootloader installation
- *4*: Failure
   - Failure to handle TUF metadata: Check logs for more information
- *6*: Failure
   - There is no Target in the device TUF repo that matches a device tag and/or hardware ID
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *20*: Failure
   - There is no Target that matches the specified name or version
- *21*: Failure
   - Unable to find Target to rollback to after a failure to start Apps at boot on a new version of sysroot
- *30*: Failure
   - Unable to perform operation: there is an installation that needs completion
- *50*: Failure
   - Target download data not found. Make sure to call pull operation first
- *110*: Failure
   - Installation failed, rollback done successfully
- *120*: Failure
   - Installation failed, rollback initiated but requires reboot to finalize
- *130*: Failure
   - Installation failed and rollback operation was not successful
- *1*: Failure
   - An error occurred while running the command. Check logs for more information


.. _ref-aklite-command-line-interface-rollback:

### rollback

> ⚠️
The rollback command is in beta stage,
and is subject to change.

The ``rollback`` command can be used to cancel the current installation and revert the system to the previous successfully installed Target.
No download operation is done in that case.

If there is no installation being done, the current running Target is marked as failing.
This avoids having it automatically installed again,
and an installation of the previous successful Target is performed.
In that situation, the installation is preceded by a download (pull) operation.

Like in a regular installation, the exit code can be used to identify if a reboot is required in to finalize the rollback.

#### Exit Codes

- *0*: Success
   - Rollback executed successfully. No reboot required
- *100*: Success
   - Rollback installation started successfully. Reboot required
- *5*: Success
   - Rollback installation started successfully. Reboot required to update bootloader
- *8*: Failure
   - Failed to find the OSTree commit and/or all Apps of the Target to be installed in the provided source bundle (offline mode only)
- *21*: Failure
   - Unable to find Target to rollback to after a failure to start Apps at boot on a new version of sysroot
- *50*: Failure
   - Unable to download Target
- *60*: Failure
   - There is not enough free space to download the Target
- *70*: Failure
   - The pulled Target content is invalid: App compose file is invalid
- *110*: Failure
   - Rollback failed, reverted back to previous running version
- *120*: Failure
   - Rollback failed, reverting back to previous running version. A reboot is required
- *130*: Failure
   - Rollback failed, and failed to revert back to previous running version
- *1*: Failure
   - An error occurred while running the command. Check logs for more information

## Automating the use of CLI Operations

The individual command line interface operations,
especially ``check``, ``pull``, ``install`` and ``run``,
can be used to automate an update flow like to the one implemented by the main *aktualizr-lite* daemon,
while allowing for limited customizations.

This `sample bash script
<https://raw.githubusercontent.com/foundriesio/sotactl/main/scripts/aklite-cli-example.sh>`_
illustrates the usage of CLI operations and proper return codes handling.

## Creating Custom Logic for Update Decision

The exit code of ``check`` and ``list`` commands
can be used to decide if an ``update`` should be performed,
as exemplified in the previous section.
By using this code,
a script can easily use the same decision logic that is employed by aktualizr-lite daemon.

If a custom decision process is required,
the use of the JSON output of both commands is recommended, enabled with ``--json 1``.
When enabling this command line option,
additional output is suppressed by default,
and the standard output text of the command can be parsed directly.
The format of the JSON output can be relied upon when creating a custom script.
Future versions will keep compatibility with the current format.

Here is an example of JSON output.

```json
[
    {
        "apps": [
            {
                "name": "app_100",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_100@sha256:<value>"
            }
        ],
        "name": "intel-corei7-64-lmp-92",
        "version": 92
    },
    {
        "apps": [
            {
                "name": "app_100",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_100@sha256:<value>"
            },
            {
                "name": "app_200",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_200@sha256:<value>"
            },
            {
                "name": "app_300",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_300@sha256:<value>"
            }
        ],
        "current": true,
        "name": "intel-corei7-64-lmp-93",
        "version": 93
    },
    {
        "apps": [
            {
                "name": "app_100",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_100@sha256:<value>"
            },
            {
                "name": "app_200",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_200@sha256:<value>"
            },
            {
                "name": "app_300",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_300@sha256:<value>"
            }
        ],
        "name": "intel-corei7-64-lmp-94",
        "newer": true,
        "version": 94
    },
    {
        "apps": [
            {
                "name": "app_100",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_100@sha256:<value>"
            },
            {
                "name": "app_200",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_200@sha256:<value>"
            },
            {
                "name": "app_300",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_300@sha256:<value>"
            }
        ],
        "failed": true,
        "name": "intel-corei7-64-lmp-99",
        "newer": true,
        "version": 99
    },
    {
        "apps": [
            {
                "name": "app_100",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_100@sha256:<value>"
            },
            {
                "name": "app_200",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_200@sha256:<value>"
            },
            {
                "name": "app_300",
                "on": true,
                "uri": "hub.foundries.io/my-factory/app_300@sha256:<value>"
            }
        ],
        "name": "intel-corei7-64-lmp-100",
        "newer": true,
        "reason": "Updating from intel-corei7-64-lmp-93 to intel-corei7-64-lmp-100",
        "selected": true,
        "version": 100
    }
]
```

In this scenario, the ``intel-corei7-64-lmp-93`` Target is running.
Its entry is marked with ``"current" : true``.
The exit code for the command would be ``16 - Update is required -- new Target version available``,
as Target ``intel-corei7-64-lmp-100`` is available.
If we look into the Target ``intel-corei7-64-lmp-100`` entry, we can notice that is has ``"selected" : true``,
meaning this is the Target that would be selected by default by ``aktualizr-lite`` to be installed.
A ``selected`` Target always has has a ``reason`` field set as well,
which describes why this Target is supposed to be installed.
All targets with ``version`` higher than the current one
are marked as ``"newer": true``.

One additional information that the JSON output presents is if the Target is a failing Target.
I.e., the installation of this Target was attempted, but led to a rollback.
This is the case of Target ``intel-corei7-64-lmp-99``,
with  ``"failed" : true``.
A custom script could, for example, retry the installation of a failed Target,
according to arbitrary criteria.

When customizing the selection of which Target has to be installed,
the Target name, or its version, needs to be passed as a parameter.
For example, in order to attempt to install Target ``intel-corei7-64-lmp-99``,
``aktualizr-lite update intel-corei7-64-lmp-99`` or ``aktualizr-lite update 99`` would be used.

Alternatively, to have the download and install operations be performed as separate steps,
``aktualizr-lite pull 99`` followed by ``aktualizr-lite install 99``
could also be used.