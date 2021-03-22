#!/usr/bin/python3
import logging
import os
import shlex
import subprocess
from time import sleep
from typing import Dict, List, NamedTuple, Optional
from uuid import uuid4

from docker.transport.unixconn import UnixHTTPAdapter
from requests import HTTPError, session

logging.basicConfig(level="INFO", format="%(asctime)s %(levelname)s: %(message)s")
log = logging.getLogger()
logging.getLogger("requests").setLevel(logging.WARNING)


class App(NamedTuple):
    uri: str


class Target(NamedTuple):
    name: str
    sha256: str
    version: int
    apps: Dict[str, App]

    @classmethod
    def from_dict(cls, data: dict) -> "Target":
        apps: Dict[str, App] = {}
        for app_name, app in (data.get("docker_compose_apps") or {}).items():
            apps[app_name] = App(app["uri"])
        return Target(data["name"], data["ostree-sha256"], data["version"], apps)


class AkliteClient:
    def __init__(self):
        self.requests = session()
        self.requests.mount("http+unix://", UnixHTTPAdapter("/var/run/aklite.sock"))

    def refresh_config(self):
        r = self.requests.get("http+unix://localhost/config")
        r.raise_for_status()
        self._config = r.json()

    @property
    def polling_interval(self) -> int:
        return int(self._config["uptane"]["polling_sec"])

    @property
    def sota_dir(self) -> str:
        # Weird thing with how propertytree/json/toml does stuff:
        path = self._config["storage"]["path"]
        if path[0] == '"':
            # path is quoted
            path = path[1:-1]
        return path

    @property
    def configured_apps(self) -> List[str]:
        # Weird thing with how propertytree/json/toml does stuff:
        apps = self._config["pacman"].get("compose_apps") or ''
        if apps[0] == '"':
            # path is quoted
            apps = apps[1:-1]
        return [x.strip() for x in apps.split(",")]

    def download(self, target: str, correlation_id: str, reason: str):
        data = {
            "target-name": target,
            "correlation-id": correlation_id,
            "reason": reason,
        }
        r = self.requests.post("http+unix://localhost/targets/download", json=data)
        r.raise_for_status()

    def get_current(self) -> Target:
        r = self.requests.get("http+unix://localhost/targets/current")
        r.raise_for_status()
        return Target.from_dict(r.json())

    def install(self, target: str, correlation_id: str):
        data = {
            "target-name": target,
            "correlation-id": correlation_id,
        }
        r = self.requests.post("http+unix://localhost/targets/install", json=data)
        r.raise_for_status()
        if r.json().get("needs-reboot"):
            log.warning("Target installation requires reboot. Rebooting now!")
            reboot_cmd = self._config["bootloader"]["reboot_command"]
            subprocess.check_call(shlex.split(reboot_cmd))

    def send_telemetry(self):
        r = self.requests.put("http+unix://localhost/telemetry")
        r.raise_for_status()

    def targets(self) -> List[Target]:
        r = self.requests.get("http+unix://localhost/targets")
        r.raise_for_status()
        targets: List[Target] = []
        for item in r.json()["targets"]:
            targets.append(Target.from_dict(item))
        return targets


def generate_correlation_id(target: Target) -> str:
    return str(target.version) + "-" + str(uuid4())


def get_desired(target_file: str, targets: List[Target]) -> Optional[Target]:
    try:
        with open(target_file) as f:
            target_name = f.read().strip()

        for t in targets:
            if t.name == target_name:
                return t
    except FileNotFoundError:
        return None
    log.error("Target(%s) not found in targets list", target_name)
    return None


def main():
    client = AkliteClient()

    current = client.get_current()
    log.info("Current target: %s", current)

    while True:
        try:
            client.refresh_config()

            log.info("Sending telemetry data")
            client.send_telemetry()

            log.info("Checking for updates")
            targets = client.targets()
            desired_file = os.path.join(client.sota_dir, "schdesired.tgt")
            desired = get_desired(desired_file, targets)

            if desired and current.name != desired.name:
                log.info("Downloading target")
                correlation_id = generate_correlation_id(desired)
                reason = f"Upgrading from {current.name} to {desired.name}"
                client.download(desired.name, correlation_id, reason)

                log.info("Installing target")
                client.install(desired.name, correlation_id)
                current = desired
        except KeyboardInterrupt:
            raise
        except Exception as e:
            req = getattr(e, "request", None)
            res = getattr(e, "resp", None)
            if req and res:
                log.error(
                    "%s %s: %d - %s", req.method, req.url, res.status_code, res.text,
                )
            elif req:
                log.error("%s %s: %s", req.method, req.url, e)
            else:
                log.exception("Unexpected error")

        log.info("Sleeping %ds", client.polling_interval)
        sleep(client.polling_interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
