import json
import logging
import os
import random
import pytest
import requests
import stat
import subprocess
import sys
from typing import Dict, List, Optional, Set, Tuple

# docker build -t aklite-e2e-test:latest docker-e2e-test

# TODO
# - Allow 2 versions of aklite/composectl to be tested at once: e.g. aktualizr-lite-lmp95 for old version, aktualizr-lite-current for new version, making sure the transition works. 
#   UseOldAklite=True/False, OldAkluteSuffix="-lmp95", Each target indicates if it has an old or current aklite
# - Improve several tags flow
# - Randomize other aspects of the update:
#   - enabling and disabling apps



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
  InstallAppPullFailure = 80
  InstallNeedsRebootForBootFw = 90
  InstallOfflineRollbackOk = 99
  InstallNeedsReboot = 100
  InstallDowngradeAttempt = 102
  InstallRollbackOk = 110
  InstallRollbackNeedsReboot = 120
  InstallRollbackFailed = 130

logger = logging.getLogger(__name__)

user_token = os.getenv("USER_TOKEN")
if not user_token:
    logger.error("USER_TOKEN environment variable not set")
    sys.exit()

factory_name = os.getenv("FACTORY")
if not factory_name:
    logger.error("FACTORY environment variable not set")
    sys.exit()


primary_tag = os.getenv("TAG")
if not primary_tag:
    logger.error("TAG environment variable not set")
    sys.exit()


base_version: Dict[str, int] = {}
base_target_version = os.getenv("BASE_TARGET_VERSION")
if not base_target_version:
    logger.error("BASE_TARGET_VERSION environment variable not set")
    sys.exit()
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

logger.info(f"Base target version: {base_version[primary_tag]}")

aklite_path_current = "./build/src/aktualizr-lite"
composectl_path_main = "/usr/bin/composectl_main"
callback_log_path = "/var/sota/callback_log.txt"

# Test modes
offline = False
single_step = True
delay_app_install = False
prune = True
old_aklite_suffix = ""


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
        cmd = f'DEVICE_FACTORY={factory_name} lmp-device-register --api-token "{user_token}" --start-daemon 0 --tags {primary_tag}'
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

def verify_events(target_version: int, expected_events: Optional[Set[Tuple[str, Optional[bool]]]] = None, second_to_last_corr_id: bool = False):
    logger.info(f"  Verifying events for version {target_version}")
    headers = {'OSF-TOKEN': user_token}
    r = requests.get(f'https://api.foundries.io/ota/devices/{device_name}/updates/', headers=headers)
    d = json.loads(r.text)

    if second_to_last_corr_id:
        latest_update = d["updates"][1]
    else:
        latest_update = d["updates"][0]
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


def adjust_for_old_aklite() -> str:
    if not old_aklite_suffix:
        return aklite_path_current

    version = aklite_current_version()
    if version is None:
        is_old_ostree = True
    else:
        target = get_target_for_actual_version(aklite_current_version())
        old_tools_ostree = { 1, 2, 3 }
        is_old_ostree = target.ostree_image_version in old_tools_ostree
    if is_old_ostree:
        os.system(f"ln -sfn /usr/bin/composectl{old_aklite_suffix} /usr/bin/composectl")
        return "/usr/bin/aktualizr-lite" + old_aklite_suffix
    else:
        os.system("ln -sfn /usr/bin/composectl_main /usr/bin/composectl")
        return aklite_path_current

def invoke_aklite(options: List[str]):
    if offline:
        options = options + [ "--src-dir", os.path.abspath("./offline-bundles/unified/") ]
    
    os.system("ln -sfn /usr/bin/composectl_main /usr/bin/composectl")
    if "status" in options:
        aklite_path = aklite_path_current
    else:
        aklite_path = adjust_for_old_aklite()
    logger.info("  Running `" + " ".join([aklite_path] + options) + "`")
    return subprocess.run([aklite_path] + options, capture_output=True)

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
    sp = invoke_aklite(['list', '--json', '1'])
    out_json = json.loads(sp.stdout)
    target_apps = [ target["apps"] for target in out_json if target.get("current", False) ]
    # there should be only 1 current target
    assert len(target_apps) == 1
    if target_apps[0] is None:
        return []
    return [ app["name"] for app in target_apps[0] ]

def get_running_apps():
    sp = subprocess.run([composectl_path_main, "ps"], capture_output=True)
    output_lines = sp.stdout.decode('utf-8').splitlines()
    # print(output_lines)
    running_app_names = [ l.split()[0] for l in output_lines if l.split()[1] == "(running)" ]
    return running_app_names

def get_running_apps_from_status():
    sp = invoke_aklite(['status', '--json', '1'])
    out_json = json.loads(sp.stdout)
    running_app_names = [ app["name"] for app in out_json["applied_target"]["apps"] if app["running"] ]
    return running_app_names

def check_running_apps(expected_apps: Optional[List[str]]=None):
    # if no apps list is specified, all apps should be running
    if expected_apps is None:
        expected_apps = get_all_current_apps()
    logger.info(f"  Verifying running apps. {expected_apps=}")
    running_apps = get_running_apps()
    assert set(expected_apps) == set(running_apps)

    # also verify status output
    running_apps_from_status = get_running_apps_from_status()
    assert set(expected_apps) == set(running_apps_from_status)

def cleanup_tuf_metadata():
    os.system("""sqlite3 /var/sota/sql.db  "delete from meta where meta_type <> 0 or version >= 3;" ".exit" """)

def cleanup_installed_data():
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

        verify_events(target.actual_version, {
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
            assert cp.returncode == ReturnCodes.InstallRollbackOk, cp.stdout.decode("utf-8")
            verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
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

def restore_system_state():
    # Get to the starting point
    logger.info(f"Restoring base environment. Offline={offline}, SingleStep={single_step}, DelayAppsInstall={delay_app_install}, Prune={prune}...")
    write_settings()
    sys_reboot()
    cp = invoke_aklite(['run'])
    version = all_primary_tag_targets[Target.First].actual_version
    cleanup_installed_data()
    cp = invoke_aklite(['update', str(version)])
    assert cp.returncode in [ ReturnCodes.Ok, ReturnCodes.InstallNeedsReboot ], cp.stdout.decode("utf-8")
    print(cp.stdout)
    sys_reboot()
    cp = invoke_aklite(['run'])
    assert aklite_current_version() == version
    clear_callbacks_log()
    cleanup_tuf_metadata()

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

@pytest.mark.parametrize('old_aklite_suffix_', ["_lmp95"])
@pytest.mark.parametrize('single_step_', [True, False])
@pytest.mark.parametrize('delay_app_install_', [True, False])
@pytest.mark.parametrize('offline_', [True, False])
def test_incremental_updates(offline_: bool, single_step_: bool, delay_app_install_: bool, old_aklite_suffix_: str):
    global offline, single_step, delay_app_install, old_aklite_suffix
    offline = offline_
    single_step = single_step_
    delay_app_install = delay_app_install_
    old_aklite_suffix = old_aklite_suffix_
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

    write_settings(apps, prune, secondary_tag)
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
