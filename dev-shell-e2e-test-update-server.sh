#!/bin/bash -e
#
# Like dev-shell-e2e-test.sh, but for the self-hosted update-server e2e backend: brings up the
# `update-server`/`registry` profile services (see docker-e2e-test/docker-compose.yml) in
# addition to `dockerd`, and points e2e-test.py / e2e-test-create-targets-local.py at them.
# No real Foundries Factory or secrets are involved -- FACTORY/USER_TOKEN below are fixed local
# values that just need to match what docker-e2e-test/update-server/entrypoint.sh bootstraps.
#
# Usage (from the aktualizr-lite source main directory):
#   ./dev-shell-e2e-test-update-server.sh python3 docker-e2e-test/e2e-test-create-targets-local.py
#   BASE_TARGET_VERSION=1 E2E_TARGETS_LAYOUT='...' ./dev-shell-e2e-test-update-server.sh pytest docker-e2e-test/e2e-test.py

docker_dir=docker-e2e-test
docker_path=${PWD}/${docker_dir}
compose="docker compose --env-file=${docker_path}/.env.dev -f ${docker_path}/docker-compose.yml --profile update-server"

down() {
	$compose down --remove-orphans
	docker volume rm ${docker_dir}_docker-runtime
}

trap down EXIT

if [ ! -d "$PWD/.device/sysroot" ]; then
    if sudo -n true 2>/dev/null; then
        echo "Running as root or passwordless sudo user, creating restricted filesystem as device's storage"
        dd if=/dev/zero of=.device_block count=100 bs=1M
        mkfs.ext4 .device_block
        mkdir .device
        sudo mount -o loop .device_block .device
        sudo chmod a+rwx .device
    else
        echo "Running as regular user, using regular directory as device's storage"
        mkdir -p $PWD/.device/sysroot
    fi
fi

if [ -n "$CCACHE_DIR" ] ; then
        CCACHE_DIR=$(readlink -f $CCACHE_DIR)
        CCACHE_ARGS="-e CCACHE_DIR=$CCACHE_DIR"
else
        CCACHE_ARGS="-e CCACHE_DIR=$PWD/.ccache"
fi

$compose up -d update-server registry

$compose run $CCACHE_ARGS \
    -e DEV_USER=$(id -u) -e DEV_GROUP=$(id -g) \
    -e TARGET="${TARGET:-aklite-tests aktualizr-get aktualizr-lite}" \
    -e E2E_BACKEND=update-server -e UPDATE_SERVER_URL=${UPDATE_SERVER_URL:-http://update-server:8080} \
    -e FACTORY=${FACTORY:-e2e-factory} -e USER_TOKEN=${USER_TOKEN:-e2e-local-dev} -e TAG=${TAG:-main} \
    -e HARDWARE_ID=${HARDWARE_ID:-intel-corei7-64} -e BASE_TARGET_VERSION="${BASE_TARGET_VERSION:-1}" \
    -e E2E_TARGETS_LAYOUT="${E2E_TARGETS_LAYOUT}" -e AKLITE_E2E_IMAGE=${AKLITE_E2E_IMAGE} \
    aklite-e2e-test "$@"
