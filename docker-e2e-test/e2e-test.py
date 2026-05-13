from datetime import datetime, timezone
import json
import hashlib
import logging
import os
import random
import pytest
import requests
import stat
import subprocess
import sys
from typing import Dict, List, Optional, Set, Tuple


fioup_cmd = "./bin/fioup"
use_fioup = os.path.exists(fioup_cmd)

if use_fioup:
    class ReturnCodes:
        UnknownError = 1
        Ok = 0
        CheckinOkCached = 0
        CheckinFailure = 0
        OkNeedsRebootForBootFw = 0
        CheckinNoMatchingTargets = 0
        CheckinNoTargetContent = 0
        InstallAppsNeedFinalization = 0
        CheckinSecurityError = 0
        CheckinExpiredMetadata = 0
        CheckinMetadataFetchFailure = 0
        CheckinMetadataNotFound = 0
        CheckinInvalidBundleMetadata = 0
        CheckinUpdateNewVersion = 0
        CheckinUpdateSyncApps = 0
        CheckinUpdateRollback = 0
        TufTargetNotFound = 0
        RollbackTargetNotFound = 0
        InstallationInProgress = 0
        NoPendingInstallation = 0
        DownloadFailure = 0
        DownloadFailureNoSpace = 0
        DownloadFailureVerificationFailed = 0
        InstallAlreadyInstalled = 0
        InstallTargetPullFailure = 0
        InstallNeedsRebootForBootFw = 0
        InstallOfflineRollbackOk = 0
        InstallNeedsReboot = 0
        InstallDowngradeAttempt = 0
        InstallRollbackOk = 1 # fioup cli returns an error instead of actually doing a rollback
        InstallRollbackNeedsReboot = 0
        InstallRollbackFailed = 1
        CheckNoUpdate = 25
        StartFailed = 70
else:
    # Aklite CLI return codes:
    class ReturnCodes:
        UnknownError = 1
        Ok = 0
        CheckinOkCached = 3
        CheckinFailure = 4
        OkNeedsRebootForBootFw = 5
        CheckinNoMatchingTargets = 6
        CheckinNoTargetContent = 8
        InstallAppsNeedFinalization = 10
        CheckinSecurityError = 11
        CheckinExpiredMetadata = 12
        CheckinMetadataFetchFailure = 13
        CheckinMetadataNotFound = 14
        CheckinInvalidBundleMetadata = 15
        CheckinUpdateNewVersion = 16
        CheckinUpdateSyncApps = 17
        CheckinUpdateRollback = 18
        TufTargetNotFound = 20
        RollbackTargetNotFound = 21
        InstallationInProgress = 30
        NoPendingInstallation = 40
        DownloadFailure = 50
        DownloadFailureNoSpace = 60
        DownloadFailureVerificationFailed = 70
        InstallAlreadyInstalled = 75
        InstallTargetPullFailure = 80
        InstallNeedsRebootForBootFw = 90
        InstallOfflineRollbackOk = 99
        InstallNeedsReboot = 100
        InstallDowngradeAttempt = 102
        InstallRollbackOk = 110
        InstallRollbackNeedsReboot = 120
        InstallRollbackFailed = 130

        # Fioup only
        CheckNoUpdate = 25
        StartFailed = 70

logger = logging.getLogger(__name__)

user_token = os.getenv("USER_TOKEN")
if not user_token:
    pytest.fail("USER_TOKEN variable needs to be set with user token that has read access to targets of ${FACTORY} factory")

factory_name = os.getenv("FACTORY")
if not factory_name:
    pytest.fail("FACTORY variable needs to be set with the name of the factory to be used during end-to-end tests")

primary_tag: str = os.getenv("TAG", "")
if not primary_tag:
    pytest.fail("TAG variable needs to be set with the tag of the e2e test targets")

hardware_id = os.getenv("HARDWARE_ID", "intel-corei7-64")

base_version: Dict[str, int] = {}
base_target_version = os.getenv("BASE_TARGET_VERSION")
if not base_target_version:
    pytest.fail("BASE_TARGET_VERSION variable needs to be set with the first version of the e2e test targets sequence")

base_version[primary_tag] = int(base_target_version)

# secondary tag used for tests that involve tags switching
secondary_tag = os.getenv("SECONDARY_TAG")
if secondary_tag:
    secondary_base_target_version = os.getenv("SECONDARY_BASE_TARGET_VERSION")
    if secondary_base_target_version:
        base_version[secondary_tag] = int(secondary_base_target_version)

def secondary_tag_is_set():
    if not secondary_tag:
        logger.error("SECONDARY_TAG environment variable not set")
        return False

    if not base_version[secondary_tag]:
        logger.error("SECONDARY_BASE_TARGET_VERSION environment variable not set")
        return False
    return True

e2e_test_ostree_tgz = os.getenv("E2E_TEST_OSTREE_TGZ")

logger.info(f"End-to-end test environment variables:")
logger.info(f"  Factory: {factory_name}")
logger.info(f"  Tag: {primary_tag}")
logger.info(f"  Hardware ID: {hardware_id}")
if secondary_tag:
    logger.info(f"  Secondary Tag: {secondary_tag}")
else:
    logger.info(f"  Secondary Tag: not set")
logger.info(f"  Base target version: {base_version[primary_tag]}")
logger.info(f"  User Token: {user_token[:2] + '*' * (len(user_token) - 2)}")
if e2e_test_ostree_tgz:
    logger.info(f"  OSTree tar.gz: {e2e_test_ostree_tgz[:2]}...")
else:
    logger.info(f"  OSTree tar.gz: not set")

aklite_path = "./build/src/aktualizr-lite"
composectl_path = "/usr/bin/composectl"
callback_log_path = "/var/sota/callback_log.txt"
tc_path = "/usr/sbin/tc"

# Test modes
offline = False
single_step = True
delay_app_install = False
prune = True


class Target:
    First = 0
    BrokenOstree = 1
    WorkingOstree = 2
    AddFirstApp = 3
    AddMoreApps = 4
    BreakApp = 5
    UpdateBrokenApp = 6
    BrokenBuild = 7
    FixApp = 8
    UpdateWorkingApp = 9
    UpdateOstreeWithApps = 10
    BrokenOstreeWithApps = 11

    def __init__(self, version_offset: int, install_rollback: bool, run_rollback: bool, build_error: bool, ostree_image_version: int, tag: str, apps: List[str] = []):
        self.version_offset = version_offset
        self.install_rollback = install_rollback
        self.run_rollback = run_rollback
        self.build_error = build_error
        self.ostree_image_version = ostree_image_version
        self.apps = apps
        self.tag = tag
        self.actual_version = base_version[self.tag] + self.version_offset

    def __str__(self):
        return f"VersionOffset={self.version_offset} InstallRollback={self.install_rollback}, RunRollback={self.run_rollback}, BuildError={self.build_error}, OSTreeImageVersion={self.ostree_image_version}, ActualVersion={self.actual_version}"

all_apps = ["shellhttpd_base_10000", "shellhttpd_base_20000", "shellhttpd_base_30000"]
all_primary_tag_targets = {
    Target.First: Target(Target.First, False, False, False, 1, primary_tag, []),
    Target.BrokenOstree: Target(Target.BrokenOstree, True, False, False, 2, primary_tag, []),
    Target.WorkingOstree: Target(Target.WorkingOstree, False, False, False, 3, primary_tag, []),
    Target.AddFirstApp: Target(Target.AddFirstApp, False, False, False, 3, primary_tag, ["shellhttpd_base_10000"]),
    Target.AddMoreApps: Target(Target.AddMoreApps, False, False, False, 3, primary_tag, all_apps),
    Target.BreakApp: Target(Target.BreakApp, False, True, False, 3, primary_tag, all_apps),
    Target.UpdateBrokenApp: Target(Target.UpdateBrokenApp, False, True, False, 3, primary_tag, all_apps),
    Target.BrokenBuild: Target(Target.BrokenBuild, False, False, True, 3, primary_tag, all_apps),
    Target.FixApp: Target(Target.FixApp, False, False, False, 3, primary_tag, all_apps),
    Target.UpdateWorkingApp: Target(Target.UpdateWorkingApp, False, False, False, 3, primary_tag, all_apps),
    Target.UpdateOstreeWithApps: Target(Target.UpdateOstreeWithApps, False, False, False, 4, primary_tag, all_apps),
    Target.BrokenOstreeWithApps: Target(Target.BrokenOstreeWithApps, True, False, False, 5, primary_tag, all_apps),
}

# fioup has no ostree support, and a install rollback does never happen
if use_fioup:
    for t in all_primary_tag_targets:
        all_primary_tag_targets[t].install_rollback = False
        all_primary_tag_targets[t].ostree_image_version = 0

if secondary_tag:
    all_secondary_tag_targets = {
        Target.First: Target(Target.First, False, False, False, 11, secondary_tag, []),
        Target.BrokenOstree: Target(Target.BrokenOstree, True, False, False, 12, secondary_tag, []),
        Target.WorkingOstree: Target(Target.WorkingOstree, False, False, False, 13, secondary_tag, []),
        Target.AddFirstApp: Target(Target.AddFirstApp, False, False, False, 13, secondary_tag, ["shellhttpd_base_10000"]),
        Target.AddMoreApps: Target(Target.AddMoreApps, False, False, False, 13, secondary_tag, all_apps),
        Target.BreakApp: Target(Target.BreakApp, False, True, False, 13, secondary_tag, all_apps),
        Target.UpdateBrokenApp: Target(Target.UpdateBrokenApp, False, True, False, 13, secondary_tag, all_apps),
        Target.BrokenBuild: Target(Target.BrokenBuild, False, False, True, 13, secondary_tag, all_apps),
        Target.FixApp: Target(Target.FixApp, False, False, False, 13, secondary_tag, all_apps),
        Target.UpdateWorkingApp: Target(Target.UpdateWorkingApp, False, False, False, 13, secondary_tag, all_apps),
        Target.UpdateOstreeWithApps: Target(Target.UpdateOstreeWithApps, False, False, False, 14, secondary_tag, all_apps),
        Target.BrokenOstreeWithApps: Target(Target.BrokenOstreeWithApps, True, False, False, 15, secondary_tag, all_apps),
    }
else:
    all_secondary_tag_targets = {}

def get_target_for_actual_version(actual_version: int):
    for target in list(all_primary_tag_targets.values()) + list(all_secondary_tag_targets.values()):
        if target.actual_version == actual_version:
            return target
    assert False, f"Unable to find target with version {actual_version}"

def register_if_required():
    if not os.path.exists("/var/sota/client.pem"):
        user_token = os.getenv("USER_TOKEN")
        if use_fioup:
            cmd = f'{fioup_cmd} register --api-token "{user_token}" --tag {primary_tag} --factory {factory_name} --hw-id {hardware_id}'
        else:
            cmd = f'DEVICE_FACTORY={factory_name} lmp-device-register --api-token "{user_token}" --start-daemon 0 --tags {primary_tag} --hwid {hardware_id}'
        logger.info(f"Registering device...")
        output = os.popen(cmd).read().strip()
        logger.info(output)
    else:
        logger.info("Device already registered")

def get_device_name():
    # os.getenv("DEVICE_NAME", "aklite-test-device")
    cmd = "openssl x509 -noout -subject -nameopt multiline -in /var/sota/client.pem | grep commonName | sed -n 's/ *commonName *= //p'"
    device_uuid = os.popen(cmd).read().strip()
    assert len(device_uuid) == 36
    # Assuming device name == uuid
    logger.info(f"Device UUID is {device_uuid}")
    return device_uuid

register_if_required()
device_name = get_device_name()

def set_device_apps(apps: Optional[List[str]]):
    if apps is None:
        data = r"""{"reason":"Override aktualizr-lite update configuration ","files":[{"name":"z-50-fioctl.toml","value":"\n[pacman]\n","unencrypted":true,"on-changed":["/usr/share/fioconfig/handlers/aktualizr-toml-update"]}]}"""
    else:
        apps_str = ",".join(apps)
        data = r"""{"reason":"Override aktualizr-lite update configuration ","files":[{"name":"z-50-fioctl.toml","value":"\n[pacman]\n  compose_apps = \"""" + apps_str + r"""\"\n  docker_apps = \"""" + apps_str + r"""\"\n","unencrypted":true,"on-changed":["/usr/share/fioconfig/handlers/aktualizr-toml-update"]}]}"""
    url = f"https://api.foundries.io/ota/devices/{device_name}/config/?factory={factory_name}&by-uuid=1"
    print(data)
    headers = {'OSF-TOKEN': user_token}
    res = requests.patch(url, data, headers=headers)
    assert res.status_code == 201, f"Unable to update device settings: {res.status_code} {res.text}"
    logger.info(f"  Updated device apps settings in the factory: {apps=}")

def verify_events(target_version: int, expected_events: Optional[Set[Tuple[str, Optional[bool]]]] = None, second_to_last_corr_id: bool = False, min_date: Optional[datetime] = None):
    if target_version:
        logger.info(f"  Verifying events for version {target_version}")
    else:
        assert min_date is not None
        logger.info(f"  Checking that no new event was generated since {min_date}")
    headers = {'OSF-TOKEN': user_token}
    r = requests.get(f'https://api.foundries.io/ota/devices/{device_name}/updates/', headers=headers)
    d = json.loads(r.text)

    if second_to_last_corr_id:
        latest_update = d["updates"][1]
    else:
        latest_update = d["updates"][0]
    # Example event: {'correlation-id': '01K8K2DKYVS14C45SW4NNWP89Q', 'target': 'intel-corei7-64-lmp-414', 'version': '414', 'time': '2025-10-27T14:51:10Z'}
    if min_date is not None:
        # Remove microsecods as event timestamps have second resolution
        min_date = min_date.replace(tzinfo=None).replace(microsecond=0)
        update_time = datetime.strptime(latest_update["time"], "%Y-%m-%dT%H:%M:%SZ")
        if target_version:
            assert update_time >= min_date, f"Latest update time {update_time} is before expected minimum date {min_date}"
        else:
            assert update_time <= min_date, f"Latest update happened at {update_time}, but there should be no events after date {min_date}"
            return

    corr_id = latest_update["correlation-id"]
    assert int(latest_update["version"]) == target_version
    r = requests.get(f'https://api.foundries.io/ota/devices/{device_name}/updates/{corr_id}/', headers=headers)

    d_update = json.loads(r.text)
    event_list = set([ (x["eventType"]["id"], x["event"]["success"]) for x in d_update ])
    if expected_events is None:
        expected_events = {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
            ('EcuInstallationStarted', None),
            ('EcuInstallationApplied', None),
            ('EcuInstallationCompleted', True)
        }
    elif use_fioup:
        expected_events.update([
            ('UpdateInitStarted', None),
            ('UpdateInitCompleted', True),
        ])

    if use_fioup and ('EcuInstallationStarted', None) in expected_events:
        # fioup always generates 'EcuInstallationApplied' event
        expected_events.add(('EcuInstallationApplied', None))

    assert set(event_list) == set(expected_events)

def sys_reboot():
    need_reboot_path = "/var/run/aktualizr-session/need_reboot"
    if os.path.isfile(need_reboot_path):
        os.remove(need_reboot_path)

def clear_callbacks_log():
    if os.path.isfile(callback_log_path):
         os.remove(callback_log_path)

# TODO: verify additional callback variables
def verify_callback(expected_calls: List[Tuple[str, str]]):
    # fioup does not invoke callbacks
    if use_fioup:
        return
    logger.info(f"  Verifying callbacks")
    calls: List[Tuple[str, str]] = []
    if os.path.isfile(callback_log_path):
        with open(callback_log_path) as f:
            for l in f.readlines():
                j = json.loads(l)
                calls.append((j["MESSAGE"], j["RESULT"]))
        clear_callbacks_log()
    assert expected_calls == calls

def aklite_current_version():
    if use_fioup:
        sp = invoke_aklite(["status", "--format", "json"])
        if sp.returncode != 0:
            return 0
        out_json = json.loads(sp.stdout)
        target_id = out_json["current_status"].get("target_id")
        if "-" in target_id:
            return int(target_id.split("-")[-1])
        else:
            return 0
    else:
        sp = invoke_aklite(['status', '--json', '1'])
        out_json = json.loads(sp.stdout)
        version = out_json["applied_target"].get("version")
        return version

def aklite_current_version_based_on_list():
    sp = invoke_aklite(['list', '--json', '1'])
    out_json = json.loads(sp.stdout)
    count = sum(target.get("current", False) for target in out_json)
    # Make sure there is only 1 "current" version
    assert count == 1
    curr_target = next(target for target in out_json if target.get("current", False))
    return curr_target["version"]

def invoke_aklite(options: List[str], kill_after_sec: Optional[float] = None ):
    if offline:
        options = options + [ "--src-dir", os.path.abspath("./offline-bundles/unified/") ]

    cmd = aklite_path
    if use_fioup:
        if options == ['check', '--json', '1']:
            options = ["check"]
        elif len(options) >= 2 and options[0] == "install":
            # fioup does not accept versioned install: it always proceeds with previous one
            options = ["install"]
        if not "daemon" in options:
            options = [ x.replace('pull', 'fetch').replace('run', 'start') for x in options if x not in ['--install-mode=delay-app-install'] ]
        cmd = fioup_cmd

    logger.info("  Running `" + " ".join([aklite_path] + options) + "`")
    if kill_after_sec is not None:
        proc = subprocess.Popen([cmd] + options, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            outs, errs = proc.communicate(timeout=kill_after_sec)
            return subprocess.CompletedProcess(proc.args, proc.returncode, outs, errs)
        except subprocess.TimeoutExpired:
            logger.info(f"  Killing process due to timeout after {kill_after_sec} seconds")
            proc.kill()
            if os.path.isfile("/var/lock/aklite.lock"):
                os.remove("/var/lock/aklite.lock")
            outs, errs = proc.communicate()
            return subprocess.CompletedProcess(proc.args, proc.returncode, outs, errs)
    return subprocess.run([cmd] + options, capture_output=True)

def write_settings(apps: Optional[List[str]] = None, prune: bool = True, tag: Optional[str] = None):
    logger.info(f"  Updating settings. {apps=}")
    callback_file = "/var/sota/callback.sh"

    callback_content = \
"""#!/bin/sh
echo { \\"MESSAGE\\": \\"$MESSAGE\\", \\"CURRENT_TARGET\\": \\"$CURRENT_TARGET\\", \\"CURRENT_TARGET_NAME\\": \\"$CURRENT_TARGET_NAME\\", \\"INSTALL_TARGET_NAME\\": \\"$INSTALL_TARGET_NAME\\", \\"RESULT\\": \\"$RESULT\\" } >> /var/sota/callback_log.txt
"""
    with open(callback_file, "w") as f:
        f.write(callback_content)
    st = os.stat(callback_file)
    os.chmod(callback_file, st.st_mode | stat.S_IEXEC)

    if not tag:
        tag = primary_tag
    content = \
f"""
[pacman]
tags = "{tag}"
"""
    if apps is not None:
        apps_str = ",".join(apps)
        content += f"""
compose_apps = "{apps_str}"
"""
    # callback_program = "/var/sota/callback.sh"

    if not prune:
        content += "\ndocker_prune = 0\n"

    with open("/etc/sota/conf.d/z-50-fioctl.toml", "w") as f:
        f.write(content)

    sota_toml_content = ""
    with open("/var/sota/sota.toml") as f:
        sota_toml_content = f.read()

    if not "callback_program" in sota_toml_content:
        sota_toml_content = sota_toml_content.replace("[pacman]", '[pacman]\ncallback_program = "/var/sota/callback.sh"\nstorage_watermark = "95"')
        with open("/var/sota/sota.toml", "w") as f:
            f.write(sota_toml_content)

def get_all_current_apps() -> List[str]:
    if use_fioup:
        t = get_target_for_actual_version(aklite_current_version())
        return t.apps
    else:
        sp = invoke_aklite(['list', '--json', '1'])
        out_json = json.loads(sp.stdout)
        target_apps = [ target["apps"] for target in out_json if target.get("current", False) ]
        # there should be only 1 current target
        assert len(target_apps) == 1
        if target_apps[0] is None:
            return []
        return [ app["name"] for app in target_apps[0] ]

def get_running_apps():
    sp = subprocess.run([composectl_path, "ps"], capture_output=True)
    output_lines = sp.stdout.decode('utf-8').splitlines()
    # print(output_lines)
    running_app_names = [ l.split()[0] for l in output_lines if l.split()[1] == "(running)" ]
    return running_app_names

def get_running_apps_from_status():
    if use_fioup:
        sp = invoke_aklite(["status", "--format", "json"])
        out_json = json.loads(sp.stdout)
        running_app_names = [ app["name"] for app in out_json["current_status"]["apps"].values() if app["running"] ]
    else:
        sp = invoke_aklite(['status', '--json', '1'])
        out_json = json.loads(sp.stdout)
        running_app_names = [ app["name"] for app in out_json["applied_target"]["apps"] if app["running"] ]
    return running_app_names

def check_running_apps(expected_apps: Optional[List[str]]=None):
    # if no apps list is specified, all apps should be running
    if expected_apps is None:
        expected_apps = get_all_current_apps()
    logger.info(f"  Verifying running apps. {expected_apps=}")
    if not use_fioup:
        running_apps = get_running_apps()
        assert set(expected_apps) == set(running_apps)

    # also verify status output
    running_apps_from_status = get_running_apps_from_status()
    assert set(expected_apps) == set(running_apps_from_status)

def cleanup_tuf_metadata():
    if use_fioup:
        os.system("""rm -f /var/sota/targets.json; rm -rf /var/sota/tuf;""")
    else:
        os.system("""sqlite3 /var/sota/sql.db  "delete from meta where meta_type <> 0 or version >= 3;" ".exit" """)

def cleanup_installed_data():
    if use_fioup:
        os.system("""rm -f /var/sota/updates.db /etc/sota/conf.d/* /run/secrets/* /var/sota/.last*""")
    os.system("""sqlite3 /var/sota/sql.db  "delete from installed_versions;" ".exit" """)

def install_with_separate_steps(target: Target, explicit_version: bool = True, do_reboot: bool = True, do_finalize: bool = True):
    cp = invoke_aklite(['check', '--json', '1'])
    verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

    previous_target = get_target_for_actual_version(aklite_current_version())
    if target.install_rollback or target.run_rollback:
        final_target = previous_target
    else:
        final_target = target
    requires_reboot = previous_target.ostree_image_version != final_target.ostree_image_version
    if target.run_rollback:
        requires_reboot = previous_target.ostree_image_version != target.ostree_image_version

    if explicit_version:
        cp = invoke_aklite(['pull', str(target.actual_version)])
    else:
        cp = invoke_aklite(['pull'])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    verify_callback([("download-pre", ""), ("download-post", "OK")])
    verify_events(target.actual_version, {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
        })

    # cp = invoke_aklite(['install', str(get_target_version(Target.BrokenBuild))]) # not existing target
    # assert cp.returncode == ReturnCodes.TufTargetNotFound
    # verify_callback([])

    # version = 159
    # cp = invoke_aklite(['install', str(version)]) # not downloaded target
    # assert cp.returncode == ReturnCodes.InstallAppPullFailure
    # verify_callback([])

    cmd = ['install']
    if explicit_version:
        cmd.append(str(target.actual_version))
    if delay_app_install:
        cmd.append("--install-mode=delay-app-install")
    cp = invoke_aklite(cmd)
    if target.install_rollback:
        assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
        verify_callback([("install-pre", ""), ("install-post", "FAILED"), ("install-pre", ""), ("install-post", "OK")])
        verify_events(target.actual_version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', False),
        }, True)
        verify_events(final_target.actual_version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', True),
        }, False)

    elif requires_reboot: # !install_rollback
        assert cp.returncode == ReturnCodes.InstallNeedsReboot, cp.stdout.decode("utf-8")
        verify_callback([("install-pre", ""), ("install-post", "NEEDS_COMPLETION")])
        verify_events(target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        if do_reboot:
            sys_reboot()

        if do_finalize:
            cp = invoke_aklite(['run'])
            if target.run_rollback:
                assert cp.returncode == ReturnCodes.InstallRollbackNeedsReboot, cp.stdout.decode("utf-8")
                verify_callback([
                    ("install-final-pre", ""), ("install-post", "FAILED"),
                    ("install-pre", ""), ("install-post", "NEEDS_COMPLETION")])
                sys_reboot()
                cp = invoke_aklite(['run'])
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_callback([("install-final-pre", ""), ("install-post", "OK")])
                verify_events(target.actual_version, {
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', False),
                    }, True)
                verify_events(final_target.actual_version, {
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', True),
                    }, False)
            else:
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_callback([("install-final-pre", ""), ("install-post", "OK")])
                verify_events(target.actual_version, {
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', True),
                    })

    elif delay_app_install: # !install_rollback && !requires_reboot
        assert cp.returncode == ReturnCodes.InstallAppsNeedFinalization, cp.stdout.decode("utf-8")
        verify_callback([
            ("install-pre", ""), ("install-post", "NEEDS_COMPLETION")
            ])

        if use_fioup:
            # fioup has the same correlation id for fetch and install operations
            verify_events(target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        else:
            verify_events(target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        cp = invoke_aklite(['run'])

        if target.run_rollback:
            verify_callback([
                ("install-final-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
            ])
            if use_fioup:
                assert cp.returncode == ReturnCodes.StartFailed, cp.stdout.decode("utf-8")
                # fioup has the same correlation id for fetch and install operations
                verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', False),
                }, False)
                # fioup CLI does not perform auto rollback. Run update --sync-current manually instead
                cp = invoke_aklite(['update', '--sync-current'])
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_events(final_target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                }, False)
            else:
                assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
                verify_events(target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', False),
                }, True)
                verify_events(final_target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationCompleted', True),
                }, False)
        else:
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            verify_callback([("install-final-pre", ""), ("install-post", "OK")])
            if use_fioup:
                # fioup has the same correlation id for fetch and install operations
                verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                })
            else:
                verify_events(target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                })

    else: # !install_rollback && !requires_reboot && !delay_app_install
        if target.run_rollback:
            assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
            verify_callback([
                ("install-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', False),
            }, True)

            verify_events(final_target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            }, False)

        else:
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            verify_callback([("install-pre", ""), ("install-post", "OK")])
            verify_events(target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })

    if (requires_reboot and not do_reboot):
        assert aklite_current_version() == previous_target.actual_version, cp.stdout.decode("utf-8")
    else:
        assert aklite_current_version() == final_target.actual_version, cp.stdout.decode("utf-8")

    if not explicit_version:
        # Make sure we would not try a new install, after trying to install the latest one
        cp = invoke_aklite(['check', '--json', '1'])
        assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
        verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

def install_with_single_step(target: Target, explicit_version: bool = True, do_reboot: bool = True, do_finalize: bool = True):
    previous_target = get_target_for_actual_version(aklite_current_version())
    if target.install_rollback or target.run_rollback:
        final_target = previous_target
    else:
        final_target = target
    requires_reboot = previous_target.ostree_image_version != final_target.ostree_image_version
    if target.run_rollback:
        requires_reboot = previous_target.ostree_image_version != target.ostree_image_version

    cmd = ['update']
    if explicit_version:
        cmd.append(str(target.actual_version))
    if delay_app_install:
        cmd.append("--install-mode=delay-app-install")
    cp = invoke_aklite(cmd)

    if target.install_rollback:
        assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
        verify_callback([
            ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
            ("download-pre", ""), ("download-post", "OK"),
            ("install-pre", ""), ("install-post", "FAILED"),
            ("install-pre", ""), ("install-post", "OK")])
        verify_events(target.actual_version, {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', False),
        }, True)
        verify_events(final_target.actual_version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', True),
        }, False)

    elif requires_reboot: # !install_rollback
        assert cp.returncode == ReturnCodes.InstallNeedsReboot, cp.stdout.decode("utf-8")
        verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "NEEDS_COMPLETION"),
                ])
        verify_events(target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        if do_reboot:
            sys_reboot()
        if do_finalize:
            cp = invoke_aklite(['run'])
            if target.run_rollback:
                assert cp.returncode == ReturnCodes.InstallRollbackNeedsReboot, cp.stdout.decode("utf-8")
                verify_callback([
                    ("install-final-pre", ""), ("install-post", "FAILED"),
                    ("install-pre", ""), ("install-post", "NEEDS_COMPLETION")])
                sys_reboot()
                cp = invoke_aklite(['run'])
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_callback([("install-final-pre", ""), ("install-post", "OK")])
                verify_events(target.actual_version, {
                        ('EcuDownloadStarted', None),
                        ('EcuDownloadCompleted', True),
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', False),
                    }, True)
                verify_events(final_target.actual_version, {
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', True),
                    }, False)
            else:
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_callback([("install-final-pre", ""), ("install-post", "OK")])
                verify_events(target.actual_version, {
                        ('EcuDownloadStarted', None),
                        ('EcuDownloadCompleted', True),
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', True),
                    })

    elif delay_app_install: # !install_rollback && !requires_reboot
        assert cp.returncode == ReturnCodes.InstallAppsNeedFinalization, cp.stdout.decode("utf-8")
        verify_callback([
            ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
            ("download-pre", ""), ("download-post", "OK"),
            ("install-pre", ""), ("install-post", "NEEDS_COMPLETION")
            ])
        verify_events(target.actual_version, {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
            ('EcuInstallationStarted', None),
            ('EcuInstallationApplied', None),
        })

        cp = invoke_aklite(['run'])
        if target.run_rollback:
            assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
            verify_callback([
                ("install-final-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
            ])
            verify_events(target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
                ('EcuInstallationCompleted', False),
            }, True)
            verify_events(final_target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            }, False)
        else:
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            verify_callback([("install-final-pre", ""), ("install-post", "OK")])
            verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                })

    else:  # !install_rollback && !requires_reboot && !delay_app_install
        if target.run_rollback:
            verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            if use_fioup:
                assert cp.returncode == ReturnCodes.StartFailed, cp.stdout.decode("utf-8")
                verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationCompleted', False),
                }, False)
                # fioup CLI does not perform auto rollback. Run update --sync-current manually instead
                cp = invoke_aklite(['update', '--sync-current'])
                assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
                verify_events(final_target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                }, False)
            else:
                assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
                verify_events(final_target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationCompleted', True),
                }, False)

        else:
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })

    if (requires_reboot and not do_reboot):
        assert aklite_current_version() == previous_target.actual_version, cp.stdout.decode("utf-8")
    else:
        assert aklite_current_version() == final_target.actual_version, cp.stdout.decode("utf-8")

    if not explicit_version:
        # Make sure we would not try a new install, after trying to install the latest one
        cp = invoke_aklite(['check', '--json', '1'])
        assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
        verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

def install_target(target: Target, explicit_version: bool=True, do_reboot: bool=True, do_finalize: bool=True):
    if single_step:
        install_with_single_step(target, explicit_version, do_reboot, do_finalize)
    else:
        install_with_separate_steps(target, explicit_version, do_reboot, do_finalize)


def do_rollback(target: Target, requires_reboot: bool, installation_in_progress: bool):
    cp = invoke_aklite(['rollback'])

    # TODO: Check events for previous version when `requires_reboot and installation_in_progress`
    if requires_reboot:
        # allowing ReturnCodes.InstallAppsNeedFinalization to workaround an test environment limitation related to getCurrent target
        assert cp.returncode in [ReturnCodes.InstallNeedsReboot] , cp.stdout.decode("utf-8")
        if installation_in_progress:
            verify_callback([
                    ("install-post", "FAILED"),
                    ("install-pre", ""), ("install-post", "NEEDS_COMPLETION"),
                    ])
        else:
            verify_callback([
                    # ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                    ("download-pre", ""), ("download-post", "OK"),
                    ("install-pre", ""), ("install-post", "NEEDS_COMPLETION"),
                    ])

        sys_reboot()
        if installation_in_progress:
            verify_events(target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                })
        else:
            verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                })
        cp = invoke_aklite(['run'])
        # TODO: handle run_rollback
        verify_callback([("install-final-pre", ""), ("install-post", "OK")])

        if installation_in_progress:
            verify_events(target.actual_version, {
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                })
        else:
            verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                })
    else:
        assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
        if installation_in_progress:
            verify_callback([
                ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(target.actual_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })
        else:
            verify_callback([
                # ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })
    assert aklite_current_version() == target.actual_version, cp.stdout.decode("utf-8")

def create_offline_bundles():
    if not e2e_test_ostree_tgz and not os.path.exists("./offline-bundles/unified/"):
        assert False, "No OSTree repo tgz provided, and offline bundles directory does not exist. Cannot proceed with offline update tests"

    if not e2e_test_ostree_tgz:
        logger.warning("No OSTree repo tgz provided for offline bundles test, skipping offline bundles creation")
        return

    if os.path.exists("./offline-bundles/unified/"):
        logger.info("Offline bundles directory already exists, skipping offline bundles creation")
        return

    cmd = f"""
mkdir -p `readlink -f .`/e2e-test-targets/small-ostree/
echo "{e2e_test_ostree_tgz}" | base64 -d | tar -xzf - -C `readlink -f .`/e2e-test-targets/small-ostree/ --strip-components=1
mkdir -p offline-bundles
echo "Creating offline bundles for versions {base_version[primary_tag]} to {base_version[primary_tag] + 11} (skipping {base_version[primary_tag] + 7})"
for version_offset in 0 1 2 3 4 5 6 8 9 10 11; do
echo offset $version_offset;
version=$[ $version_offset + {base_version[primary_tag]} ];
echo version $version;
fioctl targets offline-update \
--ostree-repo-source=`readlink -f .`/e2e-test-targets/small-ostree/repo \
--allow-multiple-targets intel-corei7-64-lmp-$version \
-f {factory_name} offline-bundles/unified  \
--tag {primary_tag} \
--expires-in-days=1 \
--factory {factory_name} \
--token {user_token} || break;
done"""
    os.system(f"bash -x -c '{cmd}'")


def restore_system_state():
    # Get to the starting point
    global offline
    logger.info(f"Restoring base environment. Offline={offline}, SingleStep={single_step}, DelayAppsInstall={delay_app_install}, Prune={prune}...")
    if offline:
        create_offline_bundles()
    write_settings()
    sys_reboot()
    if use_fioup:
        cp = invoke_aklite(['cancel'])
    else:
        cp = invoke_aklite(['run'])
    version = all_primary_tag_targets[Target.First].actual_version
    cleanup_tuf_metadata()
    cleanup_installed_data()

    if offline:
        os.makedirs("/usr/lib/sota/tuf/ci/", exist_ok=True)
        # offline bundles miss root metadata versions 1 and 2. Fetch them manually
        for root_version in [1, 2]:
            ret = os.system(f"curl -s -H 'OSF-TOKEN: {user_token}' https://api.foundries.io/ota/repo/{factory_name}/api/v1/user_repo/{root_version}.root.json -o /usr/lib/sota/tuf/ci/{root_version}.root.json")
            assert ret == 0, f"Failed to download root metadata for offline bundles version {root_version}"

    cp = invoke_aklite(['update', str(version)])
    assert cp.returncode in [ ReturnCodes.Ok, ReturnCodes.InstallNeedsReboot ], cp.stdout.decode("utf-8")
    print(cp.stdout)
    sys_reboot()
    cp = invoke_aklite(['run'])
    assert aklite_current_version() == version
    clear_callbacks_log()
    cleanup_tuf_metadata()
    set_device_apps(None)

    if use_fioup:
        os.environ.pop("FIOUP_VERSION_UPPER_LIMIT", None)
        os.environ.pop("FIOUP_E2E_RUNONCE", None)
    else:
        logger.info("Making sure there are no targets in current DB...")
        cp = invoke_aklite(['list', '--json', '1'])
        assert cp.returncode == ReturnCodes.CheckinSecurityError, cp.stdout.decode("utf-8")

# Incremental install order
install_sequence_incremental = [
    Target.BrokenOstree,
    Target.WorkingOstree,
    Target.AddFirstApp,
    Target.AddMoreApps,
    Target.BreakApp,
    Target.UpdateBrokenApp,
    Target.BrokenBuild,
    Target.FixApp,
    Target.UpdateWorkingApp,
    Target.UpdateOstreeWithApps,
    Target.BrokenOstreeWithApps,
]

def run_test_sequence_random(updates_count: int = 20):
    restore_system_state()
    apps = None # All apps, for now
    random.seed(42) # for reproducibility

    for i in range(updates_count):
        target_version = random.choice(install_sequence_incremental)
        target = all_primary_tag_targets[target_version]
        if target.build_error: # skip this one for now
            continue

        logger.info(f"Updating to {target.actual_version} {target}. SingleStep={single_step}, Offline={offline} DelayAppsInstall={delay_app_install}")
        write_settings(apps, prune)
        install_target(target)
        check_running_apps(apps)

def run_test_sequence_incremental():
    restore_system_state()
    apps = None # All apps, for now
    for target_version in install_sequence_incremental:
        target = all_primary_tag_targets[target_version]
        if target.build_error: # skip this one for now
            continue

        logger.info(f"Updating to {target.actual_version} {target}. SingleStep={single_step}, Offline={offline} DelayAppsInstall={delay_app_install}")
        write_settings(apps, prune)
        install_target(target)
        check_running_apps(apps)

def run_test_sequence_update_to_latest():
    restore_system_state()
    apps = None # All apps, for now

    target = all_primary_tag_targets[Target.BrokenOstreeWithApps]

    # Try to install latest version, which will lead to a rollback
    write_settings(apps, prune)
    logger.info(f"Updating to latest target {target.actual_version} {target} {single_step=} {offline=}")
    install_target(target, False)
    check_running_apps(apps)

def run_test_sequence_apps_selection():
    restore_system_state()
    apps = None # All apps, for now
    target_version = Target.AddMoreApps
    target = all_primary_tag_targets[target_version]

    logger.info(f"Updating to target {target.actual_version} {target}. {single_step=} {offline=}")
    write_settings(apps, prune)
    install_target(target)
    check_running_apps(apps)

    apps = ["shellhttpd_base_10000"]
    write_settings(apps, prune)
    logger.info(f"Forcing apps sync for target {target.actual_version} {target}. {single_step=} {offline=}")
    cp = invoke_aklite(['update', str(target.actual_version)])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    check_running_apps(apps)

    apps = all_apps
    write_settings(apps, prune)
    logger.info(f"Forcing apps sync for target {target.actual_version} {target}. {single_step=} {offline=}")
    cp = invoke_aklite(['update', str(target.actual_version)])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    check_running_apps(apps)

def test_apps_selection():
    run_test_sequence_apps_selection()

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('delay_app_install_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_incremental_updates(offline_: bool, single_step_: bool, delay_app_install_: bool):
    global offline, single_step, delay_app_install
    offline = offline_
    single_step = single_step_
    delay_app_install = delay_app_install_
    run_test_sequence_incremental()

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('delay_app_install_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_random_updates(offline_: bool, single_step_: bool, delay_app_install_: bool):
    global offline, single_step, delay_app_install
    offline = offline_
    single_step = single_step_
    delay_app_install = delay_app_install_
    run_test_sequence_random()

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_update_to_latest(offline_: bool, single_step_: bool):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    run_test_sequence_update_to_latest()

def run_test_switch_tag():
    assert secondary_tag_is_set()
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    install_target(all_primary_tag_targets[Target.UpdateOstreeWithApps])
    install_target(all_primary_tag_targets[Target.BrokenOstreeWithApps], False)

    logger.info(f"Switching tag to secondary tag {secondary_tag}")
    write_settings(apps, prune, secondary_tag)

    logger.info(f"Testing rollback to target from previous tag")
    # Make sure we will rollback to previous target
    install_target(all_secondary_tag_targets[Target.BrokenOstreeWithApps], False)

    install_target(all_secondary_tag_targets[Target.UpdateOstreeWithApps])

def run_test_auto_downgrade():
    assert secondary_tag_is_set()
    auto_downgrade_enabled = True
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune, secondary_tag)
    install_target(all_secondary_tag_targets[Target.UpdateOstreeWithApps])

    write_settings(apps, prune)
    cp = invoke_aklite(['check', '--json', '1'])
    if auto_downgrade_enabled:
        assert cp.returncode == ReturnCodes.CheckinUpdateNewVersion, cp.stdout.decode("utf-8")
    else:
        assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

    install_target(all_primary_tag_targets[Target.UpdateOstreeWithApps])

def run_test_deamon_auto_downgrade():
    assert secondary_tag_is_set()
    auto_downgrade_enabled = False
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune, secondary_tag)
    install_target(all_secondary_tag_targets[Target.UpdateOstreeWithApps])

    write_settings(apps, prune)
    os.environ["AKLITE_TEST_RETURN_ON_SLEEP"] = "1"
    cp = invoke_aklite(['daemon'])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    expected_callbacks = [("check-for-update-pre", ""), ("check-for-update-post", "OK")]
    if auto_downgrade_enabled:
        expected_callbacks += [("download-pre", ""), ("download-post", "OK"), ("install-pre", ""), ("install-post", "FAILED"), ("install-pre", ""), ("install-post", "OK")]
    verify_callback(expected_callbacks)

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_tag_switch(offline_: bool, single_step_: bool):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    run_test_switch_tag()

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_auto_downgrade(offline_: bool, single_step_: bool):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    run_test_auto_downgrade()


def run_test_pull_install_different_versions():
    logger.info("Testing pull/install with different versions")
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    cp = invoke_aklite(['check', '--json', '1'])
    verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

    test_pairs = [
        # apps missing
        (all_primary_tag_targets[Target.WorkingOstree], all_primary_tag_targets[Target.AddFirstApp]),
        # ostree hash missing
        (all_primary_tag_targets[Target.UpdateOstreeWithApps], all_primary_tag_targets[Target.BrokenOstreeWithApps]),
        # apps + ostree hash missing
        (all_primary_tag_targets[Target.WorkingOstree], all_primary_tag_targets[Target.BrokenOstreeWithApps])
    ]

    for download_target, install_target in test_pairs:
        # pull one version
        cp = invoke_aklite(['pull', str(download_target.actual_version)])
        assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
        verify_callback([("download-pre", ""), ("download-post", "OK")])
        verify_events(download_target.actual_version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
            })

        # try to install other version
        cp = invoke_aklite(['install', str(install_target.actual_version)])
        assert cp.returncode == ReturnCodes.InstallTargetPullFailure, cp.stdout.decode("utf-8")

@pytest.mark.parametrize('offline_', [True, False])
def test_pull_install_different_tags(offline_: bool):
    global offline
    offline = offline_
    run_test_pull_install_different_versions()

def test_auto_downgrade_daemon():
    run_test_deamon_auto_downgrade()

def run_test_rollback(do_reboot: bool, do_finalize: bool):
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    # Install
    logger.info("Installing base target for rollback operations")
    target = all_primary_tag_targets[Target.WorkingOstree]
    install_target(target)

    # Test a rollback that does *not* require a reboot
    logger.info("Testing rollback not requiring reboot")
    target = all_primary_tag_targets[Target.AddFirstApp]
    install_target(target, True, do_reboot, do_finalize)
    do_rollback(all_primary_tag_targets[Target.WorkingOstree], False, False)

    # Test a rollback that *does* require a reboot
    logger.info("Testing rollback requiring reboot")
    target = all_primary_tag_targets[Target.UpdateOstreeWithApps]
    install_target(target, True, do_reboot, do_finalize)
    do_rollback(all_primary_tag_targets[Target.WorkingOstree], do_reboot, not do_reboot or not do_finalize)

    # Do a new rollback, go back to "First" target, from the original system stare
    logger.info("Performing additional rollback operation, on already rolled back environment")
    do_rollback(all_primary_tag_targets[Target.First], True, False)

@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('do_reboot', [True, False])
@pytest.mark.parametrize('do_finalize', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_rollback(do_reboot: bool, do_finalize: bool, offline_: bool, single_step_: bool):
    if not do_reboot and do_finalize:
        return
    global offline, single_step
    offline = offline_
    single_step = single_step_
    logger.info(f"Testing rollback {do_reboot=} {do_finalize=}")
    run_test_rollback(do_reboot, do_finalize)

def test_fioup_daemon():
    if not use_fioup:
        return
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    last_installed_version = 0
    test_sequence = [
        Target.WorkingOstree,
        Target.AddFirstApp,
        Target.AddMoreApps,
        Target.BreakApp,
        Target.UpdateBrokenApp,
        Target.FixApp,
        Target.UpdateWorkingApp,
    ]
    for target_version in test_sequence:
        set_device_apps(None)
        # running_apps_before = get_running_apps_from_status()
        target = all_primary_tag_targets[target_version]
        if target.build_error: # skip this one for now
            continue
        os.environ["FIOUP_VERSION_UPPER_LIMIT"] = str(target.actual_version)
        logger.info(f"Updating to {target.actual_version} {target}")
        min_events_time = datetime.now(timezone.utc)
        os.environ["FIOUP_E2E_RUNONCE"] = "1"
        cp = invoke_aklite(["daemon"])
        if target.run_rollback:
            logger.info(f"Target {target.actual_version} is a bad target. Verifying retry and recovery behavior")
            # If bad target, check retry and "rollback" logic.
            # No explicit rollback is actually performed, just an apps sync update of the current target after
            #  giving up on trying the new target.
            assert cp.returncode == ReturnCodes.StartFailed, cp.stdout.decode("utf-8")
            assert aklite_current_version() == last_installed_version
            # Try to update to latest (bad) target 2 more times. Total attempts before giving up is 3
            for i in range(2):
                logger.info(f"Trying update to target {target.actual_version} again, attempt {i+2}/3")
                min_events_time = datetime.now(timezone.utc)
                cp = invoke_aklite(["daemon"])
                assert cp.returncode == ReturnCodes.StartFailed, cp.stdout.decode("utf-8")
                verify_events(target.actual_version, {
                        ('EcuDownloadStarted', None),
                        ('EcuDownloadCompleted', True),
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', False),
                    }, False, min_events_time)
                assert aklite_current_version() == last_installed_version

            # No more attempts for new target, a sync update to current target should be performed now
            min_events_time = datetime.now(timezone.utc)
            logger.info(f"No more attempts to target {target.actual_version} should be performed. Trying to sync current target {last_installed_version}")
            cp = invoke_aklite(["daemon"])
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            verify_events(last_installed_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                }, False, min_events_time)
            assert aklite_current_version() == last_installed_version
            check_running_apps(apps)

            # make sure there is no update, as synched target should be already running
            min_events_time = datetime.now(timezone.utc)
            logger.info(f"Sync to current target {last_installed_version} done. Making sure no new update is performed by daemon")
            cp = invoke_aklite(["daemon"])
            # If no update is required, daemon --run-once currently returns an error, "selected target is already running"
            assert cp.returncode == ReturnCodes.CheckNoUpdate, cp.stdout.decode("utf-8")
            verify_events(0, set(), False, min_events_time)
            assert aklite_current_version() == last_installed_version
            # restore originally running apps
            check_running_apps(apps)

        else:
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            assert aklite_current_version() == target.actual_version
            verify_events(target.actual_version, {
                    ('EcuDownloadStarted', None),
                    ('EcuDownloadCompleted', True),
                    ('EcuInstallationStarted', None),
                    ('EcuInstallationApplied', None),
                    ('EcuInstallationCompleted', True),
                }, False, min_events_time)
            check_running_apps(apps)
            last_installed_version = target.actual_version
            for i in range(len(target.apps)):
                custom_apps_list = target.apps[:i]
                set_device_apps(custom_apps_list)
                logger.info(f"Testing apps list: {custom_apps_list}")
                cp = invoke_aklite(["daemon"])
                assert aklite_current_version() == target.actual_version
                verify_events(target.actual_version, {
                        ('EcuDownloadStarted', None),
                        ('EcuDownloadCompleted', True),
                        ('EcuInstallationStarted', None),
                        ('EcuInstallationApplied', None),
                        ('EcuInstallationCompleted', True),
                    }, False, min_events_time)
                check_running_apps(custom_apps_list)

def is_loopback_mount(path: str):
    try:
        # Check if it is a mountpoint
        if not os.path.ismount(path):
            return False
        # Check if /proc/mounts lists it as a loop device
        with open('/proc/mounts', 'r') as f:
            for line in f:
                if path in line and '/dev/loop' in line:
                    return True
        return False
    except subprocess.CalledProcessError:
        return False

def run_test_no_space():
    is_loopback = is_loopback_mount('/var/sota')
    if not is_loopback:
        assert False, "/var/sota is not a loopback mount point, skipping free space test execution. Device storage must be a loopback device to run this test."

    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    invoke_aklite(['check'])

    # get available bytes in /var/sota
    statvfs = os.statvfs("/var/sota")
    available_bytes = statvfs.f_bavail * statvfs.f_frsize
    logger.info(f"Available space in /var/sota: {available_bytes} bytes")
    if available_bytes > 100000000:
        assert False, "Too much free space left, test environment should be configured to have less than 100MB free space for this test to be effective"

    if available_bytes > 50000:
        # fill /var/sota with data to reduce available space to 50000 bytes, to trigger no space left error
        with open("/var/sota/fill_space", "wb") as f:
            f.write(b"\0" * (available_bytes - 50000))

    statvfs = os.statvfs("/var/sota")
    available_bytes = statvfs.f_bavail * statvfs.f_frsize
    logger.info(f"Available space in /var/sota after filling it up: {available_bytes} bytes")

    if single_step:
        cmd = ['update', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)]
        expected_success_code = ReturnCodes.InstallNeedsReboot
    else:
        cmd = ['pull', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)]
        expected_success_code = ReturnCodes.Ok
    try:
        cp = invoke_aklite(cmd)
        assert cp.returncode == ReturnCodes.DownloadFailureNoSpace, cp.stdout.decode("utf-8")
    finally:
        os.remove("/var/sota/fill_space")

    cp = invoke_aklite(cmd)
    assert cp.returncode == expected_success_code, cp.stdout.decode("utf-8")


@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_no_space(offline_: bool, single_step_: bool):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    logger.info(f"Testing no space left on device")
    run_test_no_space()


def run_test_kill_process():
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)
    try:
        cmd = [ tc_path, "qdisc", "add", "dev", "eth0", "root", "netem", "delay", "500ms" ]
        subprocess.call(cmd)
        for i in range(5):
            logger.info(f" Calling update and interrupting after {i} seconds")
            cp = invoke_aklite(['update', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)], i)
            ReturnCodeProcessKilled = -9
            assert cp.returncode == ReturnCodeProcessKilled, cp.stdout.decode("utf-8")

        subprocess.call([tc_path, "qdisc", "del", "dev", "eth0", "root"])
        logger.info(f" Calling update that should succeed now")

        cp = invoke_aklite(['update', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)])
        assert cp.returncode == ReturnCodes.InstallNeedsReboot, cp.stdout.decode("utf-8")

    finally:
        subprocess.call([tc_path, "qdisc", "del", "dev", "eth0", "root"])

def test_kill_process():
    logger.info(f"Testing kill process")
    run_test_kill_process()


def run_test_bad_network():
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    try:
        cmd = [ tc_path, "qdisc", "add", "dev", "eth0", "root", "netem", "loss", "20%" ]
        subprocess.call(cmd)
        logger.info(f" Testing update with high network error rate")
        if single_step:
            cp = invoke_aklite(['update', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)])
            assert cp.returncode == ReturnCodes.InstallNeedsReboot, cp.stdout.decode("utf-8")
        else:
            cp = invoke_aklite(['check'])
            assert cp.returncode == ReturnCodes.CheckinUpdateNewVersion, cp.stdout.decode("utf-8")
            cp = invoke_aklite(['pull', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)])
            assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
            cp = invoke_aklite(['install', str(all_primary_tag_targets[Target.UpdateOstreeWithApps].actual_version)])
            assert cp.returncode == ReturnCodes.InstallNeedsReboot, cp.stdout.decode("utf-8")
        logger.info(f" Done")
    finally:
        subprocess.call([tc_path, "qdisc", "del", "dev", "eth0", "root"])

@pytest.mark.parametrize('single_step_', [True, False])
def test_bad_network(single_step_: bool):
    global offline, single_step
    offline = False
    single_step = single_step_

    logger.info(f"Testing bad network conditions with single_step={single_step}")
    run_test_bad_network()


def corrupt_file(file_path: str, expected_hash: str):
    logger.info(f"Corrupting {file_path}")
    with open(file_path, "r+b") as f:
        f.write(b'\x0E\x0E\x0E\x0B')

    # Make sure file was corrupted
    with open(file_path, "rb") as f:
        data = f.read()
        sha256_hash = hashlib.sha256(data).hexdigest()
        assert sha256_hash != expected_hash, f"File hash mismatch expected after corruption {expected_hash}, still got {sha256_hash}"

def verify_file_integrity(file_path: str, expected_hash: str):
    logger.info(f"Verifying integrity of {file_path}")
    with open(file_path, "rb") as f:
        data = f.read()
        sha256_hash = hashlib.sha256(data).hexdigest()
        assert sha256_hash == expected_hash, f"File hash mismatch after corruption and new pull, expected {expected_hash}, got {sha256_hash}"

def test_forced_sync():
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)

    target = all_primary_tag_targets[Target.AddMoreApps]
    logger.info(f"Testing force sync for target {target.actual_version} {target}. {single_step=} {offline=}")
    install_target(target)
    check_running_apps(apps)

    # Get the current target information from `check` command, and identify the app URI
    cp = invoke_aklite(['check', '--json', '1'])
    check_result = json.loads(cp.stdout.decode("utf-8"))
    target_info = next((t for t in check_result if t['version'] == target.actual_version), None)
    assert target_info is not None, f"Target with version {target.actual_version} not found in check result"
    app_name = target.apps[0]
    app_info = next((a for a in target_info['apps'] if a['name'] == app_name), None)
    assert app_info is not None, f"App {app_name} not found in check result"
    app_uri = app_info['uri']
    logger.info(f"App URI: {app_uri}")

    # Identify a blob hash for the given application
    cp = subprocess.run([composectl_path, 'inspect', app_uri, '--format', 'json'], capture_output=True)
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    out_json = json.loads(cp.stdout.decode("utf-8"))
    blob_digest = out_json['bundle']['services'][0]['image']['manifests'][0]['config']['digest'].split(':')[1]

    # Test forced `pull` command
    logger.info("Testing corruption of pulled blob, to make sure it's re-downloaded with `pull` command")
    pulled_blob_path = f"/var/sota/reset-apps/blobs/sha256/{blob_digest}"
    corrupt_file(pulled_blob_path, blob_digest)
    cp = invoke_aklite(['pull', str(target.actual_version)])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    verify_file_integrity(pulled_blob_path, blob_digest)

    # Test forced  `install` command
    logger.info("Testing corruption of installed blob, to make sure it's re-installed with `install` command")
    installed_blob_path = f"/var/lib/docker/image/overlay2/imagedb/content/sha256/{blob_digest}"
    corrupt_file(installed_blob_path, blob_digest)
    cp = invoke_aklite(['install', str(target.actual_version)])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    verify_file_integrity(installed_blob_path, blob_digest)

    # Test forced `update` command
    logger.info("Testing corruption of both pulled and installed blobs, to make sure both are re-downloaded/re-installed with `update` command")
    corrupt_file(pulled_blob_path, blob_digest)
    corrupt_file(installed_blob_path, blob_digest)
    cp = invoke_aklite(['update', str(target.actual_version)])
    assert cp.returncode == ReturnCodes.Ok, cp.stdout.decode("utf-8")
    verify_file_integrity(pulled_blob_path, blob_digest)
    verify_file_integrity(installed_blob_path, blob_digest)

# Restores the system state, useful when running commands manually inside the e2e test environment
def test_clear_env():
    restore_system_state()
    apps = None # All apps, for now
    write_settings(apps, prune)
