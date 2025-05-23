
"""
This script creates a set of targets that match the ones expected by e2e-tests.py script.
It should be executed from the aktualizr-lite source main directory, by calling:

  python docker-e2e-test/e2e-test-create-targets.py

A requirements.txt file is provided with the Python dependencies.
It also depends on fioctl, fiopush, ostree and git executables.
USER_TOKEN, FACTORY, TAG environment variables need to be set before execution.

This script is not officially supported, and should be executed only by aktualizr-lite developers.
"""

from http import HTTPStatus
from requests.exceptions import HTTPError
import os
import requests
import shutil
import subprocess
import sys
import time
import yaml

local_dir = os.path.abspath("e2e-test-targets")
aklite_path = os.path.abspath(os.getcwd())

fiopush_cmd = "fiopush"
fioctl_cmd = "fioctl"
ostree_cmd = "ostree"

for cmd in [fiopush_cmd, fioctl_cmd, ostree_cmd]:
    if shutil.which(cmd) is None:
        print(f"{cmd} not found. Install it before running this script.")
        sys.exit()

# Experimental mode for creating apps targets without relying on CI builds
use_custom_app_targets = False
app_change_counter = 0

user_token = os.getenv("USER_TOKEN")
if not user_token:
    print("USER_TOKEN environment variable not set")
    sys.exit()

factory = os.getenv("FACTORY")
if not factory:
    print("FACTORY environment variable not set")
    sys.exit()

tag = os.getenv("TAG")
if not tag:
    print("TAG environment variable not set")
    sys.exit()

def create_ostree_repo():
        if os.path.exists(local_dir):
                raise Exception(f"{local_dir} directory already exists. Remove it before running")

        ostree_path = os.path.abspath(os.path.join(local_dir, "small-ostree"))

        os.mkdir(local_dir)
        os.mkdir(ostree_path)
        os.chdir(ostree_path)
        repo_dir = os.path.join(ostree_path, "repo")
        os.system(f"ostree --repo={repo_dir} init --mode=archive")

        tree_path = os.path.join(ostree_path, "tree")
        os.mkdir(tree_path)

        ostree_version_txt = os.path.join(tree_path, "test_ostree.txt")
        ostree_hashes = {}
        bad_ostree_versions = { 2, 5 }
        for ostree_version in range(1, 6):
                make_sys_rootfs_cmd = os.path.join(aklite_path, "tests", "make_sys_rootfs.sh")
                os.system(f"{make_sys_rootfs_cmd} {tree_path} {tag} intel-corei7-64 lmp")
                if ostree_version in bad_ostree_versions:
                        os.system("rm -rf tree/boot")

                with open(ostree_version_txt, 'w') as f:
                        f.write(f"OSTREE_{ostree_version}")

                sp = subprocess.run([ostree_cmd, "--repo=repo", "commit", "--branch=main", tree_path], capture_output=True)
                ostree_hashes[ostree_version] = sp.stdout.decode('utf-8').strip()

        for ostree_version in ostree_hashes:
                print(f"OSTREE_HASH_{ostree_version}={ostree_hashes[ostree_version]}")
        return repo_dir, ostree_hashes


def add_tag_to_ci(tag):
        os.system(f"[ -d ci-scripts ] || git clone https://source.foundries.io/factories/{factory}/ci-scripts.git")
        with open("ci-scripts/factory-config.yml") as stream:
                try:
                        ci_script_yaml = yaml.safe_load(stream)
                except yaml.YAMLError as exc:
                        print(exc)

        ci_script_yaml["lmp"]["tagging"][f"refs/heads/{tag}"] = [{'tag': tag}]
        ci_script_yaml["containers"]["tagging"][f"refs/heads/{tag}"] = [{'tag': tag}]

        with open("ci-scripts/factory-config.yml", "w") as stream:
                try:
                        yaml.safe_dump(ci_script_yaml, stream)
                except yaml.YAMLError as exc:
                        print(exc)

        print(ci_script_yaml)
        os.chdir("ci-scripts")
        os.system(f"git commit -s --no-gpg-sign factory-config.yml -m 'Adding tag {tag} to CI'")
        os.system(f"git push")
        os.chdir("..")

def set_offline_containers(enabled):
        os.system(f"[ -d ci-scripts ] || git clone https://source.foundries.io/factories/{factory}/ci-scripts.git")

        ci_script_yaml = None
        with open("ci-scripts/factory-config.yml") as stream:
                try:
                        ci_script_yaml = yaml.safe_load(stream)
                except yaml.YAMLError as exc:
                        print(exc)

        ci_script_yaml["containers"]["offline"] = {"enabled": enabled}
        with open("ci-scripts/factory-config.yml", "w") as stream:
                try:
                        yaml.safe_dump(ci_script_yaml, stream)
                except yaml.YAMLError as exc:
                        print(exc)

        print(ci_script_yaml)
        os.chdir("ci-scripts")
        os.system(f"git commit -s --no-gpg-sign factory-config.yml -m 'Setting offline containers enabled = {enabled}'")
        os.system(f"git push")
        os.chdir("..")

def write_file(filename, content):
        with open(filename, "w") as stream:
                stream.write(content)


def create_app(base_http_port, http_instances_count, reference_app_base_port, break_build=False, script_suffix="", message_prefix=""):
        # script_suffix is used to break execution of app

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

        with open("docker-build.conf", "w") as stream:
                stream.write("""# Allow CI loop to unit test the container by running a command inside it
TEST_CMD="/bin/true"
""")

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
    image: hub.foundries.io/{factory}/{ref_app_name}:latest
    restart: always
    command: /usr/local/bin/shellhttpd_cmd_{i}{script_suffix}.sh
    ports:
      - {base_http_port + i}:${{PORT-8080}}
    environment:
      MSG: "${{MSG-{message_prefix}Hello world from e2e test port {base_http_port + i}}}"
"""

        if not reference_app_base_port and use_custom_app_targets:
                os.system(f"docker build . -t hub.foundries.io/{factory}/{app_name}:latest")
                os.system("docker login hub.foundries.io")
                os.system(f"docker push hub.foundries.io/{factory}/{app_name}:latest")

        write_file("docker-compose.yml",
f"""version: '3.2'

services:
{services_str}
""")

        if use_custom_app_targets:
                global app_change_counter
                app_change_counter += 1
                composectl_cmd = "composectl"
                os.system(f"{composectl_cmd} publish hub.foundries.io/{factory}/{app_name}:{tag}-{app_change_counter} -d app.hash")

        os.chdir("..")

def add_target(hash, tag, factory, first=False):
        if first:
                src_tag = "main"
        else:
                src_tag = tag
        cmd = [fioctl_cmd, "targets", "add", "--type", "ostree", "--tags", tag, "--src-tag", src_tag, "intel-corei7-64", hash, "--factory", factory]
        sp = subprocess.run(cmd, capture_output=True)
        output = sp.stdout.decode('utf-8').strip()
        lines = output.splitlines()
        version_line = [ x for x in lines if x.strip().startswith('"version":') ]
        if not version_line:
                return 0
        else:
                return int(version_line[0].split(":")[1].strip('" '))

def push_apps(message, tag):
        if use_custom_app_targets:
                apps = []
                subdirs = [f.path.strip("./") for f in os.scandir(".") if f.is_dir()]
                for app_name in subdirs:
                        hash = ""
                        with open(f"{app_name}/app.hash", "r") as hash_file:
                                hash = hash_file.read()
                        apps.append(f"hub.foundries.io/{factory}/{app_name}@{hash}")

                cmd = [fioctl_cmd, "targets", "add", "--type", "app", "--tags", tag, "--src-tag", tag, "--factory", factory ] + apps
                subprocess.run(cmd)
        else:
                os.system(f"git add *; git commit --no-gpg-sign -m '{message}' && git push origin {tag}")

def retrieve_api_request(url, token, method="get", **kwargs):
        authentication = {
                "OSF-TOKEN": token,
        }
        retries = 3
        retry_codes = [
                HTTPStatus.TOO_MANY_REQUESTS,
                HTTPStatus.INTERNAL_SERVER_ERROR,
                HTTPStatus.BAD_GATEWAY,
                HTTPStatus.SERVICE_UNAVAILABLE,
                HTTPStatus.GATEWAY_TIMEOUT,
        ]

        requests_method = getattr(requests, method)
        call_kwargs = {
                "headers": authentication
        }
        call_kwargs.update(kwargs)

        for n in range(retries):
                try:
                        build_request = requests_method(url, **call_kwargs)
                        build_request.raise_for_status()
                        return build_request.json()
                except HTTPError as exc:
                        code = exc.response.status_code
                        if code in retry_codes:
                                # retry after n seconds
                                time.sleep(n)
                                continue
                        raise

def get_api_builds(factory, user_token):
        # Get the first page only (which contains the latest builds)
        domain = "foundries.io"
        url = f"https://api.{domain}/projects/{factory}/lmp/builds/"
        return retrieve_api_request(url, user_token).get("data")

def wait_jobs_execution(factory, user_token):
        print(f"Waiting for jobs in factory {factory} to finish")
        last_logged_pending_builds = None
        while True:
                reply = get_api_builds(factory, user_token)
                if not reply:
                        time.sleep(5)
                        continue

                pending_builds = [ x for x in reply["builds"] if x['status'] in {"RUNNING", "RUNNING_WITH_FAILURES", "PROMOTED", "QUEUED"} or [ y for y in x["runs"] if y["status"] in {"RUNNING", "RUNNING_WITH_FAILURES", "PROMOTED", "QUEUED"} ]  ]
                if pending_builds:
                        if last_logged_pending_builds != pending_builds:
                                print(f"Pending builds ({len(pending_builds)}): {pending_builds}\n")
                                last_logged_pending_builds = pending_builds
                        if len(pending_builds) == 1:
                                time.sleep(10)
                        else:
                                time.sleep(60)
                else:
                        break
        print(f"Done waiting for jobs in factory {factory} to finish")

if __name__ == "__main__":
        repo_dir, ostree_hashes = create_ostree_repo()

        os.system(f"{fiopush_cmd} -factory {factory} -repo {repo_dir} -token {user_token}")

        os.chdir(local_dir)

        add_tag_to_ci(tag)

        wait_jobs_execution(factory, user_token)

        set_offline_containers(False)

        # Dummy target, just to make sure we have a source target with the required tag
        add_target(ostree_hashes[1], tag, factory, True)

        if use_custom_app_targets:
                os.system(f"[ -d containers ] || mkdir -p containers")
        else:
                os.system(f"[ -d containers ] || git clone https://source.foundries.io/factories/{factory}/containers.git")
        os.chdir("containers")
        if not use_custom_app_targets:
                os.system(f"git checkout {tag} || git checkout -B {tag}")

        os.system("git rm -r *")
        push_apps("Clear all apps", tag)

        wait_jobs_execution(factory, user_token)

        base_target_version = add_target(ostree_hashes[1], tag, factory)
        add_target(ostree_hashes[2], tag, factory)
        add_target(ostree_hashes[3], tag, factory)

        os.chdir("..")
        set_offline_containers(True)

        os.chdir("containers")
        create_app(10000, 5, None)
        push_apps("Add first app", tag)

        create_app(20000, 1, None)
        create_app(30000, 2, 10000)
        push_apps("Add more app", tag)

        create_app(20000, 1, None, False, "_wrong")
        push_apps("Break shellhttpd base 20000 app", tag)

        create_app(20000, 1, None, False, "_still_wrong", tag)
        push_apps("Break shellhttpd base 20000 app again", tag)

        create_app(20000, 1, None, True)
        push_apps("Break shellhttpd base 20000 app build", tag)

        create_app(20000, 1, None,)
        push_apps("Fixing shellhttpd base 20000 app", tag)

        create_app(20000, 1, None, False, "", "Updated ")
        push_apps("Updating shellhttpd base 20000 app", tag)

        wait_jobs_execution(factory, user_token)

        add_target(ostree_hashes[4], tag, factory)

        add_target(ostree_hashes[5], tag, factory)

        print(f"""
Test targets successfully created

# Required environment variables for e2e tests:
export FACTORY={factory}
export TAG={tag}
export USER_TOKEN={user_token}
export BASE_TARGET_VERSION={base_target_version}

# Create offline bundles:"
mkdir -p offline-bundles
for version_offset in 0 1 2 3 4 5 6 8 9 10 11; do
version=$[ $version_offset + $BASE_TARGET_VERSION ];
echo $version;
fioctl targets offline-update --ostree-repo-source={local_dir}/small-ostree/repo --allow-multiple-targets intel-corei7-64-lmp-$version -f ${{FACTORY}} offline-bundles/unified  --tag ${{TAG}} || break;
done

# Run tests:
./dev-shell-e2e-test.sh pytest docker-e2e-test/e2e-test.py
""")
