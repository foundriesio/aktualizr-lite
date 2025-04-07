#!/bin/sh -e

if [ -z $DEV_USER ] || [ -z $DEV_GROUP ]; then
    echo "DEV_USER and DEV_GROUP environment variables must be set."
    exit 1
fi

# Create a group with the specified GID if it doesn't already exist
if ! getent group $DEV_GROUP >/dev/null; then
    groupadd -g $DEV_GROUP devgrp
fi

# Create a user with the specified UID and GID if it doesn't already exist
if ! getent passwd $DEV_USER >/dev/null; then
    useradd -u $DEV_USER -g $DEV_GROUP -m dev
fi

# Change ownership of the home directory to the appuser
chown -R dev:devgrp /home/dev

chown dev:devgrp /var/run/docker/docker.sock

chown -R dev:devgrp /var/sota
chown -R dev:devgrp /usr/lib/sota/conf.d
chown -R dev:devgrp /etc/sota/conf.d
chown -R dev:devgrp /var/lib/docker

# Initialize ostree
if [ ! -d /sysroot/ostree/repo ]; then
    echo "Initializing sysroot ostree..."
    ostree admin init-fs /sysroot
    ostree admin os-init lmp
    ostree config set core.mode bare-user
    ${PWD}/tests/make_sys_rootfs.sh initfs lmp intel-corei7-64 lmp
    commit=$(ostree commit initfs --branch lmp)
    ostree admin deploy --os=lmp $commit
    rm -rf initfs
    chown -R dev:devgrp /ostree
    ostree config set core.mode bare-user-only
fi
if [ ! -d /etc/ostree ]; then
    mkdir /etc/ostree
    chown -R dev:devgrp /etc/ostree
    chown -R dev:devgrp /boot
fi

ln -sfn ${PWD}/build/aktualizr/src/aktualizr_get/aktualizr-get /usr/local/bin/aktualizr-get

# Initialize default toml config
sysroot_cfg=/usr/lib/sota/conf.d/z-90-sysroot.toml
if [ ! -f $sysroot_cfg ]; then
    echo "[pacman]\nbooted = 0\nos = \"lmp\"" > $sysroot_cfg
fi

bootloader_cfg=/usr/lib/sota/conf.d/z-91-bootloader.toml
if [ ! -f $bootloader_cfg ]; then
    echo "[bootloader]\nreboot_command = \"/usr/bin/true\"" > $bootloader_cfg
fi

# Set directory used for reboot indication, the `need_reboot` file is created under this dir
if [ ! -d /var/run/aktualizr-session ]; then
	mkdir -p /var/run/aktualizr-session
	chown dev:devgrp /var/run/aktualizr-session
	chmod 700 /var/run/aktualizr-session
fi

# Run the command as the created user
exec gosu $DEV_USER:$DEV_GROUP "$@"
