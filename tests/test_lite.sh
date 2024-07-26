#!/usr/bin/env bash
set -ex


build_dir=$(pwd)
aklite=$1
uptane_gen_bin=$2
tests_dir=$3
compose_bin=$4
#valgrind=$4
valgrind=""
docker_daemon=$5

dest_dir=$(mktemp -d)

cleanup() {
    echo "cleaning up temp dir"
    rm -rf "$dest_dir"
    if [ -n "$pid" ] ; then
        echo "killing webserver"
        kill $pid
    fi
    if [ -n "${daemon_pid}" ] ; then
      echo "killing docker daemon mock: "${daemon_pid}""
      kill "${daemon_pid}"
    fi
    if [ -n "${daemon_sock}" ] && [ -r "${daemon_sock}" ]; then
      rm -f "${daemon_sock}"
    fi
    if [ -n "${daemon_dir}" ] && [ -d "${daemon_dir}" ]; then
      rm -rf "${daemon_dir}"
    fi
}
trap cleanup EXIT

start_docker_daemon_mock() {
  daemon_dir=$(mktemp -d)
  daemon_sock=$(mktemp -u)
  "${docker_daemon}" -d "${daemon_dir}" -u "${daemon_sock}" &
  daemon_pid=$!
  export DOCKER_HOST="unix://${daemon_sock}"
  echo "docker daemon mock is listening on ${daemon_sock}"
}

uptane_gen() {
    $uptane_gen_bin --repotype image --path "$dest_dir" "$@"
}

add_target() {
    local custom_json="${dest_dir}/custom.json"
    local version=$1
    local name=$2
    if [ -n "$3" ] ; then
        sha=$3
    else
        sha=$(echo $name | sha256sum | cut -f1 -d\  )
    fi
    if [ -n "$4" ] ; then
        tag="\"$4\""
    fi
    cat >$custom_json <<EOF
{
  "version": "$1",
  "hardwareIds": ["hwid-for-test"],
  "containers-sha": "deadbeef",
  "tags": [$tag],
  "targetFormat": "OSTREE"
}
EOF
    uptane_gen --command image \
               --targetname $name --targetsha256 $sha --targetlength 0 \
               --hwid hwid-for-test --targetcustom $custom_json
}

start_docker_daemon_mock
uptane_gen --command generate --expires 2041-07-04T16:33:27Z
add_target 1000 foo1
add_target 1001 foo2
uptane_gen --command signtargets

pushd $dest_dir
python3 -m http.server 0&
pid=$!
port=$("$tests_dir/find_listening_port.sh" "$pid")
echo "http server listening on $port"

export OSTREE_SYSROOT=$dest_dir/sysroot
mkdir $OSTREE_SYSROOT
$tests_dir/ostree-scripts/makephysical.sh $OSTREE_SYSROOT

sota_dir=$dest_dir/sota
mkdir $sota_dir
chmod 700 $sota_dir
cat >$sota_dir/sota.toml <<EOF
[uptane]
repo_server = "http://localhost:$port/repo/repo"

[provision]
primary_ecu_hardware_id = "hwid-for-test"

[storage]
type = "sqlite"
path = "$sota_dir"
sqldb_path = "sql.db"
uptane_metadata_path = "$sota_dir/metadata"

[pacman]
type = "ostree+compose_apps"
sysroot = "$OSTREE_SYSROOT"
os = "dummy-os"
docker_compose_bin = "${compose_bin}"
docker_bin = "/bin/true"
booted = "staged"
docker_images_reload_cmd = "/bin/true"
compose_apps_root = "$sota_dir/compose-apps"
compose_apps_tree = "$sota_dir/apps-tree"
enforce_composeapp_engine = ""
EOF

## Check that we can do the info command
$valgrind $aklite -h | grep "finalize"
$valgrind $aklite -h | grep "run"
$valgrind $aklite -h | grep "status"
$valgrind $aklite -h | grep "list"
$valgrind $aklite -h | grep "check"
$valgrind $aklite -h | grep "update"
$valgrind $aklite -h | grep "pull"
$valgrind $aklite -h | grep "install"
$valgrind $aklite -h | grep "daemon"


## Check that we can do the list command
out=$($valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml list)
if [[ ! "$out" =~ "foo1" ]] ; then
    echo "ERROR: foo1 update missing"
    exit 1
fi
if [[ ! "$out" =~ "foo2" ]] ; then
    echo "ERROR: foo2 update missing"
    exit 1
fi

## Check that we can do the update command
update=$(ostree admin status | head -n 1)
version=1002
name="zlast"  # give a name that will cause the custom version to be the latest
sha=$(echo $update | cut -d\  -f2 | sed 's/\.0$//')
echo "Adding new target: $version $name / $sha"
add_target $version $name $sha


$valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml update --update-name $name
ostree admin status

$valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml update | grep "Target $name is already installed"


out=$($valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml status)
if [[ ! "$out" =~ "Active image is: $version	sha256:$sha" ]] ; then
    echo "ERROR: status incorrect:"
    echo $out
    exit 1
fi
source ${sota_dir}/current-target
[ "$TARGET_NAME" = "zlast" ] || (echo current-target wrong: $TARGET_NAME != zlast; exit 1)
[ "$CONTAINERS_SHA" = "deadbeef" ] || (echo current-target wrong: $CONTAINERS_SHA != deadbeef; exit 1)

## Make sure we obey tags and notify the callback program
promoted_version=1003
add_target $promoted_version promoted-$name $sha promoted
echo 'tags = "promoted"' >> $sota_dir/sota.toml
echo "callback_program = \"${sota_dir}/callback.sh\"" >> $sota_dir/sota.toml
cat >$sota_dir/callback.sh <<EOF
#!/bin/sh -e
env >> ${sota_dir}/\$MESSAGE.log
EOF
chmod +x $sota_dir/callback.sh
$valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml update | grep "To New Target: promoted-$name"
for callback in download-pre download-post install-pre ; do
  if [ -f ${sota_dir}/${callback}.log ] ; then
    grep "INSTALL_TARGET_NAME=promoted-zlast" ${sota_dir}/${callback}.log
    grep "CURRENT_TARGET_NAME=zlast" ${sota_dir}/${callback}.log
  else
    echo "ERROR: Callback not performed for $callback"
    exit 1
  fi
done
callback=install-post
if [ -f ${sota_dir}/${callback}.log ] ; then
    grep "INSTALL_TARGET_NAME=promoted-zlast" ${sota_dir}/${callback}.log
    grep "CURRENT_TARGET_NAME=promoted-zlast" ${sota_dir}/${callback}.log
    grep "RESULT=OK" ${sota_dir}/${callback}.log
  else
    echo "ERROR: Callback not performed for $callback"
    exit 1
fi

cat >>$sota_dir/sota.toml <<EOF
[bootloader]
reboot_command = "/bin/true"
EOF

AKLITE_TEST_RETURN_ON_SLEEP=1 $valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml daemon | grep "Device is up-to-date"

promoted_version=1004
add_target $promoted_version promoted2-$name "" promoted
AKLITE_TEST_RETURN_ON_SLEEP=1 $valgrind $aklite --loglevel 1 -c $sota_dir/sota.toml daemon | grep "Going to install"
