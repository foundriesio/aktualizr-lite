#!/bin/bash

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

mkdir -p $PWD/.device/sysroot
docker compose --env-file=${docker_path}/.env.dev -f ${docker_path}/docker-compose.yml run -e DEV_USER=$(id -u) -e DEV_GROUP=$(id -g) -e BASE_TARGET_VERSION=${BASE_TARGET_VERSION} -e USER_TOKEN=${USER_TOKEN} aklite-e2e-test "$@"
