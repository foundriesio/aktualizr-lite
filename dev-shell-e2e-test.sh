#!/bin/bash -e

docker_dir=docker-e2e-test
docker_path=${PWD}/${docker_dir}

# Function to execute custom commands before exiting
down() {
	docker compose --env-file=${docker_path}/.env.dev -f ${docker_path}/docker-compose.yml down --remove-orphans
	# remove the docker runtime part
    docker volume rm ${docker_dir}_docker-runtime
}

# Register the cleanup function to be called on EXIT
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

docker compose --env-file=${docker_path}/.env.dev -f ${docker_path}/docker-compose.yml run $CCACHE_ARGS -e DEV_USER=$(id -u) -e DEV_GROUP=$(id -g) -e BASE_TARGET_VERSION=${BASE_TARGET_VERSION} -e USER_TOKEN=${USER_TOKEN} -e TAG=${TAG} -e E2E_TEST_OSTREE_TGZ="${E2E_TEST_OSTREE_TGZ}" -e SECONDARY_TAG=${SECONDARY_TAG} -e SECONDARY_BASE_TARGET_VERSION="${SECONDARY_BASE_TARGET_VERSION}" -e SECONDARY_E2E_TEST_OSTREE_TGZ="${SECONDARY_E2E_TEST_OSTREE_TGZ}" -e AKLITE_E2E_IMAGE=${AKLITE_E2E_IMAGE} aklite-e2e-test "$@"
