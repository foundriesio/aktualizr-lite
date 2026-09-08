
"""
This script creates the same target sequence as e2e-test-create-targets.py, but against a
self-hosted update-server instance (see docker-e2e-test/docker-compose.yml, profile
`update-server`) instead of a real Foundries Factory. It should be executed from the
aktualizr-lite source main directory:

  python docker-e2e-test/e2e-test-create-targets-local.py

Unlike e2e-test-create-targets.py, this needs no FACTORY/USER_TOKEN and does not talk to git or
Foundries CI at all:
 - ostree commits are built locally exactly as before (create_ostree_repo(), unchanged).
 - compose apps are built and pushed to the local throwaway `registry` service and published
   with `composectl`, instead of git-pushed to a Foundries "containers" repo and built by
   Foundries CI.
 - targets are created with `fiocli updates upload --version <N>` (an explicit version number)
   instead of `fioctl targets add` (whose version is whatever the Factory assigns next). This
   also means, unlike the Foundries flow, there is nothing to "reserve" a version slot for the
   BrokenBuild target: e2e-test.py never installs it (`if target.build_error: continue`), so
   this script simply never uploads anything for that offset.

Requires `docker`, `ostree`, `composectl`, and `fiocli` on PATH, and a running
update-server + registry (`docker compose --profile update-server up -d update-server registry`).

This script is not officially supported, and should be executed only by aktualizr-lite developers.
"""

import json
import os
import shutil
import subprocess
import sys
from typing import Dict, List, Optional, Tuple

local_dir = os.path.abspath("e2e-test-targets")
aklite_path = os.path.abspath(os.getcwd())
apps_store_dir = os.path.join(local_dir, "apps_store")

docker_cmd = "docker"
ostree_cmd = "ostree"
composectl_cmd = "composectl"
fiocli_cmd = "fiocli"

for cmd in [docker_cmd, ostree_cmd, composectl_cmd, fiocli_cmd]:
    if shutil.which(cmd) is None:
        print(f"{cmd} not found. Install it before running this script.")
        sys.exit(1)

tag = os.getenv("TAG", "main")
hardware_id = os.getenv("HARDWARE_ID", "intel-corei7-64")
registry_host = os.getenv("REGISTRY_HOST", "registry:5000")
update_server_url = os.getenv("UPDATE_SERVER_URL", "http://update-server:8080")
# fiocli --version is explicit for this backend (unlike the Factory, which assigns the next
# version itself), so any fixed starting point works; only needs to be unique per fiocli
# context/tag combination, which a fresh update-server data dir already guarantees.
base_target_version = int(os.getenv("BASE_TARGET_VERSION", "1"))

fiocli_context = "e2e-local"


def run_cmd(cmd: str, success_required: bool = True) -> str:
    print(f"Running command: {cmd}")
    sp = subprocess.run(cmd, shell=True, capture_output=True)
    if sp.returncode != 0 and success_required:
        raise Exception(f"\nCommand '{cmd}' failed with error:\n{sp.stderr.decode('utf-8')}")
    return sp.stdout.decode('utf-8').strip()


def create_ostree_repo() -> Tuple[str, str, Dict[int, str], str]:
        if os.path.exists(local_dir):
                raise Exception(f"{local_dir} directory already exists. Remove it before running")

        ostree_path = os.path.abspath(os.path.join(local_dir, "small-ostree"))

        os.mkdir(local_dir)
        os.mkdir(ostree_path)
        os.chdir(ostree_path)
        repo_dir = os.path.join(ostree_path, "repo")
        run_cmd(f"ostree --repo={repo_dir} init --mode=archive")

        tree_path = os.path.join(ostree_path, "tree")
        os.mkdir(tree_path)

        ostree_version_txt = os.path.join(tree_path, "test_ostree.txt")
        ostree_hashes: Dict[int, str] = {}
        bad_ostree_versions = { 2, 5 }
        for ostree_version in range(1, 6):
                make_sys_rootfs_cmd = os.path.join(aklite_path, "tests", "make_sys_rootfs.sh")
                run_cmd(f"{make_sys_rootfs_cmd} {tree_path} {tag} {hardware_id} lmp")
                if ostree_version in bad_ostree_versions:
                        boot_path = os.path.join(tree_path, "boot")
                        if os.path.exists(boot_path):
                                run_cmd(f"rm -rf {boot_path}")

                with open(ostree_version_txt, 'w') as f:
                        f.write(f"OSTREE_{ostree_version}")

                sp = subprocess.run([ostree_cmd, "--repo=repo", "commit", "--generate-sizes", "--branch=main", tree_path], capture_output=True)
                ostree_hashes[ostree_version] = sp.stdout.decode('utf-8').strip()

        os.chdir("..")

        # A deliberately large ostree commit (version 6), in a SEPARATE repo, used by the online
        # pre-pull size-check e2e tests. See e2e-test-create-targets.py's create_ostree_repo() for
        # the full rationale (unchanged here).
        big_ostree_path = os.path.abspath(os.path.join(local_dir, "big-ostree"))
        os.mkdir(big_ostree_path)
        os.chdir(big_ostree_path)
        big_repo_dir = os.path.join(big_ostree_path, "repo")
        run_cmd(f"ostree --repo={big_repo_dir} init --mode=archive")
        big_tree_path = os.path.join(big_ostree_path, "tree")
        os.mkdir(big_tree_path)
        run_cmd(f"{make_sys_rootfs_cmd} {big_tree_path} {tag} {hardware_id} lmp")
        with open(os.path.join(big_tree_path, "test_ostree.txt"), 'w') as f:
                f.write("OSTREE_6")
        run_cmd(f"dd if=/dev/urandom of={os.path.join(big_tree_path, 'big.img')} bs=1M count=20")
        sp = subprocess.run([ostree_cmd, "--repo=repo", "commit", "--generate-sizes", "--branch=main", big_tree_path], capture_output=True)
        ostree_hashes[6] = sp.stdout.decode('utf-8').strip()
        run_cmd(f"rm -f {os.path.join(big_tree_path, 'big.img')}")
        os.chdir("..")

        for ostree_version in ostree_hashes:
                print(f"OSTREE_HASH_{ostree_version}={ostree_hashes[ostree_version]}")

        return repo_dir, big_repo_dir, ostree_hashes, ""


def write_file(filename: str, content: str):
        with open(filename, "w") as stream:
                stream.write(content)


app_publish_counter = 0


def create_app(base_http_port: int, http_instances_count: int, reference_app_base_port: Optional[int],
                break_build: bool = False, script_suffix: str = "", message_prefix: str = "") -> Optional[Tuple[str, str]]:
        """Writes the app's files locally (same layout as e2e-test-create-targets.py's
        create_app()), builds+pushes its image (unless reusing another app's image via
        reference_app_base_port), and publishes+pulls it through the local registry.

        Returns (app_name, app_hash), or None if break_build (the build is expected to fail --
        e2e-test.py never installs this target, so there is nothing further to publish).
        """
        global app_publish_counter

        app_name = f"shellhttpd_base_{base_http_port}"
        if not os.path.exists(app_name):
                os.mkdir(app_name)
        os.chdir(app_name)

        if reference_app_base_port:
                ref_app_name = f"shellhttpd_base_{reference_app_base_port}"
        else:
                ref_app_name = app_name
                docker_file_str = """FROM alpine
COPY shellhttpd_*.sh /usr/local/bin/
"""
                if break_build:
                        docker_file_str += "BREAKING_APPS_BUILD!\n"
                write_file("Dockerfile", docker_file_str)

        http_script = \
"""#!/bin/sh -e

PORT="${PORT-8080}"
MSG="${MSG-OK-$0}"

RESPONSE="HTTP/1.1 200 OK\r\n\r\n${MSG}\r\n"

while true; do
        echo -en "$RESPONSE" | nc -l -p "${PORT}" || true
        echo "= $(date) ============================="
done
"""

        services_str = ""
        for i in range(1, http_instances_count+1):
                if not reference_app_base_port:
                        write_file(f"shellhttpd_cmd_{i}.sh", http_script)
                        os.chmod(f"shellhttpd_cmd_{i}.sh", 0o775)

                services_str += \
f"""
  httpd_{i}:
    image: {registry_host}/{ref_app_name}:latest
    restart: always
    command: /usr/local/bin/shellhttpd_cmd_{i}{script_suffix}.sh
    ports:
      - {base_http_port + i}:${{PORT-8080}}
    environment:
      MSG: "${{MSG-{message_prefix}Hello world from e2e test port {base_http_port + i}}}"
"""

        write_file("docker-compose.yml",
f"""version: '3.2'

services:
{services_str}
""")

        if not reference_app_base_port:
                image_ref = f"{registry_host}/{ref_app_name}:latest"
                if break_build:
                        # The build is *expected* to fail (BREAKING_APPS_BUILD!): don't use
                        # run_cmd(), which raises on any non-zero exit.
                        sp = subprocess.run([docker_cmd, "build", ".", "-t", image_ref], capture_output=True)
                        assert sp.returncode != 0, "expected the build_error app build to fail, but it succeeded"
                        os.chdir("..")
                        return None
                run_cmd(f"{docker_cmd} build . -t {image_ref}")
                run_cmd(f"{docker_cmd} push {image_ref}")

        app_publish_counter += 1
        app_hash_file = "app.hash"
        digest = run_cmd(
            f"{docker_cmd} inspect {registry_host}/{ref_app_name}:latest --format '{{{{index .RepoDigests 0}}}}'"
        )
        # RepoDigests is "<host>/<repo>@sha256:<hash>" -- keep just the "@sha256:<hash>" suffix.
        pinned_digest = digest[digest.index("@"):]
        # Published under the same repo name as the raw image (just a different tag, "vN" vs
        # "latest") so the app's own name matches the bare "shellhttpd_base_<port>" convention
        # other parts of e2e-test.py hardcode (e.g. run_test_sequence_apps_selection()'s
        # apps = ["shellhttpd_base_10000"]), instead of picking a divergent local-only name.
        run_cmd(
            f"{composectl_cmd} publish -d {app_hash_file} "
            f"--pinned-images {registry_host}/{ref_app_name}{pinned_digest} "
            f"{registry_host}/{app_name}:v{app_publish_counter} amd64"
        )
        with open(app_hash_file) as f:
                app_hash = f.read().strip()

        run_cmd(f"{composectl_cmd} pull -i {apps_store_dir} -s {apps_store_dir} "
                f"{registry_host}/{app_name}@{app_hash}")

        os.chdir("..")
        return (app_name, app_hash)


def make_upload_dir(name: str, ostree_repo_src: str, apps: Dict[str, str]) -> str:
        """Assembles the offline-update-style directory `fiocli updates upload` expects: a copy
        of the (shared, multi-commit) `ostree_repo/` -- the specific commit is pinned separately
        via --ostree-hash, since the repo dir is the same across many targets -- and an `apps/`
        directory containing only the specific app versions given in `apps` (not the whole
        accumulated apps_store history -- fiocli probes whatever it finds under `apps/apps/`)."""
        upload_dir = os.path.join(local_dir, "uploads", name)
        os.makedirs(upload_dir, exist_ok=True)
        shutil.copytree(ostree_repo_src, os.path.join(upload_dir, "ostree_repo"))

        if apps:
                blobs_src = os.path.join(apps_store_dir, "blobs")
                if os.path.exists(blobs_src):
                        shutil.copytree(blobs_src, os.path.join(upload_dir, "apps", "blobs"))
                for app_name, app_hash in apps.items():
                        # apps_store's directory names are the bare hex digest (no "sha256:"
                        # prefix), unlike the app.hash/pull-URI form used elsewhere.
                        hash_hex = app_hash.split(":", 1)[-1]
                        src = os.path.join(apps_store_dir, "apps", app_name, hash_hex)
                        dst = os.path.join(upload_dir, "apps", "apps", app_name, hash_hex)
                        shutil.copytree(src, dst)

        return upload_dir


def upload_target(name: str, version: int, ostree_repo_dir: str, ostree_hash: str, apps: Dict[str, str]):
        upload_dir = make_upload_dir(name, ostree_repo_dir, apps)
        target_name = f"{hardware_id}-lmp-{version}"
        cmd = [fiocli_cmd, "updates", "upload", tag, name, upload_dir,
               "--version", str(version), "--hardware-id", hardware_id,
               "--name", target_name, "--ostree-hash", ostree_hash]
        print(f"Running command: {' '.join(cmd)}")
        subprocess.run(cmd, check=True)


all_apps = ["shellhttpd_base_10000", "shellhttpd_base_20000", "shellhttpd_base_30000"]

targets_layout = {
    "targets": {
        "First":               {"offset": 0,  "ostree_version": 1, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": []},
        "BrokenOstree":        {"offset": 1,  "ostree_version": 2, "install_rollback": True,  "run_rollback": False, "build_error": False, "apps": []},
        "WorkingOstree":       {"offset": 2,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": []},
        "AddFirstApp":         {"offset": 3,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": ["shellhttpd_base_10000"]},
        "AddMoreApps":         {"offset": 4,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": all_apps},
        "BreakApp":            {"offset": 5,  "ostree_version": 3, "install_rollback": False, "run_rollback": True,  "build_error": False, "apps": all_apps},
        "UpdateBrokenApp":     {"offset": 6,  "ostree_version": 3, "install_rollback": False, "run_rollback": True,  "build_error": False, "apps": all_apps},
        "BrokenBuild":         {"offset": 7,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": True,  "apps": all_apps},
        "FixApp":              {"offset": 8,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": all_apps},
        "UpdateWorkingApp":    {"offset": 9,  "ostree_version": 3, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": all_apps},
        "UpdateOstreeWithApps":{"offset": 10, "ostree_version": 4, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": all_apps},
        "BigOstree":           {"offset": 11, "ostree_version": 6, "install_rollback": False, "run_rollback": False, "build_error": False, "apps": []},
        "BrokenOstreeWithApps":{"offset": 12, "ostree_version": 5, "install_rollback": True,  "run_rollback": False, "build_error": False, "apps": all_apps},
    },
    "offline_bundle_offsets": [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 12],
}


if __name__ == "__main__":
        repo_dir, big_repo_dir, ostree_hashes, _ = create_ostree_repo()

        # fiocli writes ~/.config/satcli.yaml on login but doesn't create the directory itself;
        # a fresh container has no ~/.config at all.
        os.makedirs(os.path.expanduser("~/.config"), exist_ok=True)
        run_cmd(f"{fiocli_cmd} login {fiocli_context} {update_server_url} --token e2e-local-dev")

        os.chdir(local_dir)
        os.makedirs("containers", exist_ok=True)

        def v(offset: int) -> int:
                return base_target_version + offset

        # First, BrokenOstree, WorkingOstree: ostree-only, no apps. All small-ostree targets
        # share the same repo dir (one repo, several commits) -- the specific commit for each
        # is pinned via ostree_hashes[N], not by which repo dir is passed.
        upload_target("First", v(0), repo_dir, ostree_hashes[1], {})
        upload_target("BrokenOstree", v(1), repo_dir, ostree_hashes[2], {})
        upload_target("WorkingOstree", v(2), repo_dir, ostree_hashes[3], {})

        # AddFirstApp: introduce shellhttpd_base_10000.
        os.chdir("containers")
        app_10000, hash_10000 = create_app(10000, 5, None)
        os.chdir("..")
        upload_target("AddFirstApp", v(3), repo_dir, ostree_hashes[3], {app_10000: hash_10000})

        # AddMoreApps: add 20000 (own image) and 30000 (reuses 10000's image).
        os.chdir("containers")
        app_20000, hash_20000 = create_app(20000, 1, None)
        app_30000, hash_30000 = create_app(30000, 2, 10000)
        os.chdir("..")
        apps_v4 = {app_10000: hash_10000, app_20000: hash_20000, app_30000: hash_30000}
        upload_target("AddMoreApps", v(4), repo_dir, ostree_hashes[3], apps_v4)

        # BreakApp / UpdateBrokenApp: 20000's *command* references a script file that isn't
        # actually written (script_suffix), so the container fails at runtime, not build time.
        os.chdir("containers")
        app_20000, hash_20000 = create_app(20000, 1, None, False, "_wrong")
        os.chdir("..")
        upload_target("BreakApp", v(5), repo_dir, ostree_hashes[3],
                       {**apps_v4, app_20000: hash_20000})

        os.chdir("containers")
        app_20000, hash_20000 = create_app(20000, 1, None, False, "_still_wrong")
        os.chdir("..")
        upload_target("UpdateBrokenApp", v(6), repo_dir, ostree_hashes[3],
                       {**apps_v4, app_20000: hash_20000})

        # BrokenBuild: 20000's Dockerfile itself is broken (BREAKING_APPS_BUILD!). e2e-test.py
        # never installs this target (`if target.build_error: continue`), so nothing is
        # uploaded for it -- offset 7 is simply skipped.
        os.chdir("containers")
        result = create_app(20000, 1, None, True)
        assert result is None, "expected the build_error app build to fail, but it succeeded"
        os.chdir("..")

        # FixApp / UpdateWorkingApp: 20000 working again.
        os.chdir("containers")
        app_20000, hash_20000 = create_app(20000, 1, None)
        os.chdir("..")
        apps_v8 = {**apps_v4, app_20000: hash_20000}
        upload_target("FixApp", v(8), repo_dir, ostree_hashes[3], apps_v8)

        os.chdir("containers")
        app_20000, hash_20000 = create_app(20000, 1, None, False, "", "Updated ")
        os.chdir("..")
        apps_v9 = {**apps_v4, app_20000: hash_20000}
        upload_target("UpdateWorkingApp", v(9), repo_dir, ostree_hashes[3], apps_v9)

        # UpdateOstreeWithApps / BigOstree / BrokenOstreeWithApps: ostree-only changes from here
        # on, apps stay at whatever UpdateWorkingApp last published.
        upload_target("UpdateOstreeWithApps", v(10), repo_dir, ostree_hashes[4], apps_v9)
        upload_target("BigOstree", v(11), big_repo_dir, ostree_hashes[6], {})
        upload_target("BrokenOstreeWithApps", v(12), repo_dir, ostree_hashes[5], apps_v9)

        targets_layout_json = json.dumps(targets_layout, separators=(",", ":"))

        print(f"""
Test targets successfully created against {update_server_url}

# Required environment variables for e2e tests:
export FACTORY=e2e-local
export TAG={tag}
export USER_TOKEN=e2e-local-dev
export BASE_TARGET_VERSION={base_target_version}
export E2E_TARGETS_LAYOUT='{targets_layout_json}'

# Run tests:
./dev-shell-e2e-test.sh pytest docker-e2e-test/e2e-test.py
""")

        output_file = os.getenv('GITHUB_OUTPUT')
        if output_file:
                with open(output_file, 'a') as f:
                        f.write(f"BASE_TARGET_VERSION={base_target_version}\n")
                        f.write(f"E2E_TARGETS_LAYOUT={targets_layout_json}\n")
