#!/usr/bin/python3

import sys
import logging
import os
import subprocess
import yaml
import traceback
import json


logger = logging.getLogger("Fake Docker Compose")


def up(out_dir, app_name, compose, flags):
    logger.info("Up: " + flags[0] + " " + flags[1])

    if not compose["x-status"]["valid"]:
        exit(1)

    logger.info("Run services...")
    with open(os.path.join(out_dir, "containers.json"), "r") as f:
        containers = json.load(f)

    for service in compose["services"]:
        logger.info("Run service " + service)

        def find_existing_container(config_hash):
            for c in containers:
                if c["Labels"]["io.compose-spec.config-hash"] == config_hash:
                    return c
            return None

        container = find_existing_container(compose["services"][service]["labels"]["io.compose-spec.config-hash"])
        if container is None:
            container = {"Labels": {}}
            container["Labels"]["com.docker.compose.project"] = app_name
            container["Labels"]["com.docker.compose.service"] = service
            container["Labels"]["io.compose-spec.config-hash"] = compose["services"][service]["labels"][
                "io.compose-spec.config-hash"]
            container["Image"] = compose["services"][service]["image"]
            container["State"] = "running" if "-d" in flags else "created"
            containers.append(container)
        else:
            container["State"] = "running" if "-d" in flags else "created"

    with open(os.path.join(out_dir, "containers.json"), "w") as f:
        json.dump(containers, f)


def main():
    exit_code = 0
    try:
        compose_file_path = os.path.join(os.getcwd(), "docker-compose.yml")
        with open(compose_file_path) as f:
            compose = yaml.safe_load(f)

        out_dir = sys.argv[1]
        cmd = sys.argv[2]
        logger.info("Command: " + cmd)

        app_name = os.path.basename(os.getcwd())
        if cmd == "up":
            up(out_dir, app_name, compose, sys.argv[3:])
        elif cmd == "config":
            exit_code = subprocess.call(["docker-compose", "config"], timeout=10)
    except Exception as exc:
        logger.error("Failed to process compose file: {}\n{}".format(exc, traceback.format_exc()))
        exit_code = 1

    return exit_code


if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)
    sys.exit(main())
