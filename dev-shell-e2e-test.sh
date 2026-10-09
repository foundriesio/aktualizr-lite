#!/bin/bash -e
#
# With E2E_BACKEND=update-server, also start the local update-server and registry and point
# the tests at them, with fixed FACTORY/USER_TOKEN so no Factory credentials reach them.

docker_dir=docker-e2e-test
docker_path=${PWD}/${docker_dir}
# Compose prefixes volume names with the project name, which COMPOSE_PROJECT_NAME can override.
project=${COMPOSE_PROJECT_NAME:-${docker_dir}}
compose="docker compose --env-file=${docker_path}/.env.dev -f ${docker_path}/docker-compose.yml"

backend_args=()
if [ "$E2E_BACKEND" = "update-server" ]; then
	compose="$compose --profile update-server"
	export FACTORY=e2e-factory USER_TOKEN=e2e-local-dev
	TAG=${TAG:-main}
	BASE_TARGET_VERSION=${BASE_TARGET_VERSION:-1}
	backend_args=(-e E2E_BACKEND=update-server -e BASE_TARGET_VERSION=$BASE_TARGET_VERSION
		-e UPDATE_SERVER_URL=${UPDATE_SERVER_URL:-http://update-server:8080}
		-e HARDWARE_ID=${HARDWARE_ID:-intel-corei7-64} -e TARGET="${TARGET:-aklite-tests aktualizr-get aktualizr-lite}")
fi

# Function to execute custom commands before exiting
down() {
	$compose down --remove-orphans
	# remove the docker runtime part
	docker volume rm ${project}_docker-runtime
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

if [ "$E2E_BACKEND" = "update-server" ]; then
	$compose up -d update-server registry
fi

$compose run $CCACHE_ARGS -e DEV_USER=$(id -u) -e DEV_GROUP=$(id -g) -e USER_TOKEN=${USER_TOKEN} -e TAG=${TAG} -e E2E_TEST_OSTREE_TGZ="${E2E_TEST_OSTREE_TGZ}" -e E2E_TARGETS_LAYOUT="${E2E_TARGETS_LAYOUT}" -e SECONDARY_TAG=${SECONDARY_TAG} -e SECONDARY_E2E_TEST_OSTREE_TGZ="${SECONDARY_E2E_TEST_OSTREE_TGZ}" -e AKLITE_E2E_IMAGE=${AKLITE_E2E_IMAGE} -e USE_SOFTHSM=${USE_SOFTHSM} ${SOFTHSM_PIN:+-e SOFTHSM_PIN=$SOFTHSM_PIN} ${SOFTHSM_SO_PIN:+-e SOFTHSM_SO_PIN=$SOFTHSM_SO_PIN} "${backend_args[@]}" aklite-e2e-test "$@"
