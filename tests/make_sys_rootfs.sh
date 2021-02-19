#!/bin/bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  echo "Usage: ./make_sys_rootfs.sh <rootfs-dir> <branch> <hardware-id> <os-name>"
  exit 1
fi

TARGETDIR=$1
BRANCHNAME=$2
HARDWARE=$3
OS=$4

#usr
mkdir -p "$TARGETDIR/usr/share/sota"
echo "$BRANCHNAME" > "$TARGETDIR/usr/share/sota/branchname"

#var
mkdir -p "$TARGETDIR/var/sota"

#etc
mkdir -p "$TARGETDIR/usr/etc"
echo "$HARDWARE" > "$TARGETDIR/usr/etc/hostname"
cat << EOF > "$TARGETDIR/usr/etc/os-release"
ID="${OS}"
NAME="Generated OSTree-enabled OS"
VERSION="3.14159"
EOF

#boot
mkdir -p "$TARGETDIR/boot/loader.0"
mkdir -p "$TARGETDIR/boot/loader.1"

ln -sf boot/loader.0 "$TARGETDIR/boot/loader"
echo "I'm a kernel" > "$TARGETDIR/boot/vmlinuz"
echo "I'm an initrd" > "$TARGETDIR/boot/initramfs"

checksum=$(sha256sum "$TARGETDIR/boot/vmlinuz" | cut -f 1 -d " ")
mv "$TARGETDIR/boot/vmlinuz" "$TARGETDIR/boot/vmlinuz-$checksum"
mv "$TARGETDIR/boot/initramfs" "$TARGETDIR/boot/initramfs-$checksum"
