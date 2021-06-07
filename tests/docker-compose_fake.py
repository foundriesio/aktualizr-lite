#!/usr/bin/python3

import sys
import logging
import os
import yaml
import traceback
import json


logger = logging.getLogger("Fake Docker Compose")


def up(out_dir, app_name, compose, flags):
    logger.info("Up: " + flags[0] + " " + flags[1])
    if "-d" not in flags:
        return

    logger.info("Run services...")
    with open(os.path.join(out_dir, "containers.json"), "r") as f:
        containers = json.load(f)

    for service in compose["services"]:
        container = {"Labels": {}}
        logger.info("Run service " + service)
        container["Labels"]["com.docker.compose.project"] = app_name
        container["Labels"]["com.docker.compose.service"] = service
        container["Labels"]["io.compose-spec.config-hash"] = compose["services"][service]["labels"]["io.compose-spec.config-hash"]
        containers.append(container)

    with open(os.path.join(out_dir, "containers.json"), "w") as f:
        json.dump(containers, f)


def pull(out_dir, app_name, compose):
    logger.info("Pull images...")
    with open(os.path.join(out_dir, "images.json"), "r") as f:
        images = json.load(f)

    for service in compose["services"]:
        image = {"RepoDigests": []}
        image_url = compose["services"][service]["image"]
        logger.info("Pull image: " + image_url)
        image["RepoDigests"].append(image_url)
        images.append(image)

    with open(os.path.join(out_dir, "images.json"), "w") as f:
        json.dump(images, f)


def main():
    exit_code = 0
    try:
        compose_file_path = os.path.join(os.getcwd(), "docker-compose.yml")
        with open(compose_file_path) as f:
            compose = yaml.safe_load(f)

        out_dir = sys.argv[1]
        cmd = sys.argv[2]
        logger.info("Command: " + cmd)
        flags = sys.argv[3:]
        if len(flags) > 2 and flags[1] == '--no-start':
            logger.info(">>>>>>>>>>>> --no-start")
            cmd = ""
        app_name = os.path.basename(os.getcwd())
        if cmd == "up":
            up(out_dir, app_name, compose, sys.argv[3:])
        elif cmd == "pull":
            pull(out_dir, app_name, compose)
    except Exception as exc:
        logger.error("Failed to process compose file: {}\n{}".format(exc, traceback.format_exc()))
        exit_code = 1

    return exit_code


if __name__ == "__main__":
    logging.basicConfig(level=logging.DEBUG)
    sys.exit(main())
