#!/usr/bin/python3
from argparse import ArgumentParser
import logging
import shlex
import subprocess
import sys
from time import sleep
from typing import Dict, List, NamedTuple, Optional
from uuid import uuid4

from docker.transport.unixconn import UnixHTTPAdapter
from requests import HTTPError, session

logging.basicConfig(level="INFO", format="%(asctime)s %(levelname)s: %(message)s")
log = logging.getLogger()
logging.getLogger("requests").setLevel(logging.WARNING)


class Target(NamedTuple):
    name: str
    sha256: str
    version: int

    @classmethod
    def from_dict(cls, data: dict) -> "Target":
        return Target(data["name"], data["ostree-sha256"], data["version"])


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

    def get_current(self) -> Target:
        r = self.requests.get("http+unix://localhost/targets/current")
        r.raise_for_status()
        return Target.from_dict(r.json())

    def check_in(self) -> List[Target]:
        r = self.requests.get("http+unix://localhost/check_in")
        r.raise_for_status()
        targets: List[Target] = []
        for item in r.json()["targets"]:
            targets.append(Target.from_dict(item))
        return targets

    def check_apps_in_sync(self) -> Optional[str]:
        r = self.requests.get("http+unix://localhost/apps_in_sync")
        r.raise_for_status()
        if r.status_code != 204:
            return r.json()["installer-id"]
        return None

    def create_installer(self, target_name: str) -> str:
        data = {"target-name": target_name}
        r = self.requests.post("http+unix://localhost/targets/installer", json=data)
        r.raise_for_status()
        return r.json()["installer-id"]

    def download(self, installer_id: str):
        r = self.requests.post(
            f"http+unix://localhost/targets/installer/installer_id/download"
        )
        r.raise_for_status()

    def install(self, installer_id: str):
        r = self.requests.post(
            f"http+unix://localhost/targets/installer/installer_id/install"
        )
        r.raise_for_status()
        if r.status_code == 202:
            reboot_cmd = self._config["bootloader"]["reboot_command"]
            # weirdness in boost::ptree serialization
            if reboot_cmd[0] == '"':
                reboot_cmd = reboot_cmd[1:]
            if reboot_cmd[-1] == '"':
                reboot_cmd = reboot_cmd[:-1]
            log.warning(
                "Target installation requires reboot({reboot_cmd}). Rebooting now!"
            )
            subprocess.check_call(shlex.split(reboot_cmd))


def _get_parser():
    parser = ArgumentParser(description="Example client to aktualizr-lite rest api")
    sub = parser.add_subparsers(help="sub-command help")

    p = sub.add_parser("daemon", help="Run as normal OTA update agent")
    p.set_defaults(func=main_daemon)

    p = sub.add_parser("list", help="List targets available to this device")
    p.set_defaults(func=main_list)

    p = sub.add_parser("status", help="Show the current device status")
    p.set_defaults(func=main_status)

    return parser


def main_list(args):
    client = AkliteClient()
    targets = client.check_in()
    for t in targets:
        print(t)


def main_status(args):
    client = AkliteClient()
    current = client.get_current()
    latest = client.check_in()[-1]
    print("Current target:", current.name, "/", current.sha256)
    if current.version != latest.version:
        print("Newer target awaiting install", latest.name, "/", latest.sha256)
    else:
        print("Device is up to date")


def main_daemon(args):
    client = AkliteClient()

    current = client.get_current()
    log.info("Current target: %s", current)

    while True:
        try:
            client.refresh_config()

            log.info("Checking in")
            latest = client.check_in()[-1]
            log.info("Latest target is %s", latest)

            if current.name != latest.name:
                log.info("Creating installation context")
                installer_id = self.create_installer(latest.name)
                log.info("Downloading target")
                client.download(installer_id)
                log.info("Installing target")
                client.install(installer_id)
                current = latest
            else:
                installer_id = client.check_apps_in_sync()
                if installer_id:
                    log.info("Syncing active target apps")
                    log.info("Downloading target apps")
                    client.download(installer_id)
                    log.info("Installing target apps")
                    client.install(installer_id)

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
    parser = _get_parser()
    args = parser.parse_args()
    if getattr(args, "func", None):
        try:
            args.func(args)
        except KeyboardInterrupt:
            pass
    else:
        parser.print_help(sys.stderr)
        sys.exit(1)
