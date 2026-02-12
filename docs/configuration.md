# Configuration

## Configuration Update Methods

* Editing `/var/sota/sota.toml` on a device
* Adding or editing an existing configuration snippet, e.g. `/etc/sota/conf.d/z-50-fioctl-01.toml` on a device
* Running `fioctl devices config <device>` from a host.
  See [Configuring Devices](https://docs.foundries.io/latest/reference-manual/ota/configuring.html#ref-configuring-devices) for more details.

## Parameters

The following are aktualizr-repo's configuration parameters that can be useful to modify.
The presented values are the default ones.

```yaml
[uptane]
# Target/Update check-in interval
polling_sec = 300

[pacman]
# A comma separated list of Compose Apps to update
compose_apps = ""

# Compose Apps root directory
compose_apps_root = "/var/sota/compose-apps"

# Prune/Delete unused docker containers and images
docker_prune = "1"

# A comma separated list of Tags to look for in Targets that should be applied to a given device
tags = "master"

# The param instructs aktualizr-lite to (re-)create App containers of a new Target just before reboot if set to "1" (default).
# If the param is set to "0" then the App containers are (re-)created just after a successful boot on a new ostree version during aklite startup.
create_containers_before_reboot = "0"

# A percentage of an available storage that can be used by Compose Apps.
# aktualizr-lite checks whether there is enough storage available before performing OTA update of Compose Apps.
# min(sizeof(AppsV_N+1) - sizeof(AppsV_N), 0)  <  <available_storage> * <storage_watermark>/100
# storage_watermark value has to be between 20 and 95
# By default, if the configuration param is not specified, it is set to "80".
storage_watermark = "60" (set to "80" if not specified)

[logger]
# Set log level 0-5 (trace, debug, info, warning, error, fatal)
loglevel = 2

[bootloader]
# A command to invoke after an ostree repo update in order to reboot a system and apply the update
reboot_command = "/bin/true"
```
