import json
import logging
import os
import pytest
import requests
import stat
import subprocess
import sys

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
if not user_token:
    logger.error("FACTORY environment variable not set")
    sys.exit()

base_target_version = os.getenv("BASE_TARGET_VERSION")
if not base_target_version:
    logger.error("BASE_TARGET_VERSION environment variable not set")
    sys.exit()

base_target_version = int(base_target_version)
logger.info(f"Base target version: {base_target_version}")

aklite_path = "./build/src/aktualizr-lite"
composectl_path = "/usr/bin/composectl"
callback_log_path = "/var/sota/callback_log.txt"

# Test modes
offline = False
single_step = True
prune = True

def get_target_version(offset):
    return base_target_version + offset

class Targets:
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

    def __init__(self, version_offset, install_rollback, run_rollback, build_error, ostree_image_version, apps = []):
        self.version_offset = version_offset
        self.install_rollback = install_rollback
        self.run_rollback = run_rollback
        self.build_error = build_error
        self.ostree_image_version = ostree_image_version
        self.apps = apps

    def __str__(self):
        return f"VersionOffset={self.version_offset} InstallRollback={self.install_rollback}, RunRollback={self.run_rollback}, BuildError={self.build_error}, OSTreeImageVersion={self.ostree_image_version}"

all_apps = ["shellhttpd", "shellhttpd2", "shellhttpd_base_port_30000"]
all_targets = {
    Targets.First: Targets(Targets.First, False, False, False, 1, []),
    Targets.BrokenOstree: Targets(Targets.BrokenOstree, True, False, False, 2, []),
    Targets.WorkingOstree: Targets(Targets.WorkingOstree, False, False, False, 3, []),
    Targets.AddFirstApp: Targets(Targets.AddFirstApp, False, False, False, 3, ["shellhttpd"]),
    Targets.AddMoreApps: Targets(Targets.AddMoreApps, False, False, False, 3, all_apps),
    Targets.BreakApp: Targets(Targets.BreakApp, False, True, False, 3, all_apps),
    Targets.UpdateBrokenApp: Targets(Targets.UpdateBrokenApp, False, True, False, 3, all_apps),
    Targets.BrokenBuild: Targets(Targets.BrokenBuild, False, False, True, 3, all_apps),
    Targets.FixApp: Targets(Targets.FixApp, False, False, False, 3, all_apps),
    Targets.UpdateWorkingApp: Targets(Targets.UpdateWorkingApp, False, False, False, 3, all_apps),
    Targets.UpdateOstreeWithApps: Targets(Targets.UpdateOstreeWithApps, False, False, False, 4, all_apps),
    Targets.BrokenOstreeWithApps: Targets(Targets.BrokenOstreeWithApps, True, False, False, 5, all_apps),
}

def register_if_required():
    if not os.path.exists("/var/sota/client.pem"):
        user_token = os.getenv("USER_TOKEN")
        cmd = f'DEVICE_FACTORY={factory_name} lmp-device-register --api-token "{user_token}" --start-daemon 0 --tags main'
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

def verify_events(target_version, expected_events = None, second_to_last_corr_id = False):
    logger.info(f"Verifying events for version {target_version}")
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
def verify_callback(expected_calls):
    logger.info(f"Verifying callbacks")
    calls = []
    if os.path.isfile(callback_log_path):
        with open(callback_log_path) as f:
            for l in f.readlines():
                j = json.loads(l)
                calls.append((j["MESSAGE"], j["RESULT"]))
        clear_callbacks_log()
    assert expected_calls == calls

def aklite_current_version():
    sp = invoke_aklite(['status'])
    # Last line should be like this:
    # info: Active image is: 42    sha256:1f5c2258e6e493741c719394bd267b2e163609f1cb3457ccb71fcaf770c5c116
    lines = [ x for x in sp.stdout.decode('utf-8').splitlines() if "Active image is:" in x ]
    assert len(lines) == 1
    version = int(lines[0].split()[3])
    return version

def aklite_current_version_based_on_list():
    sp = invoke_aklite(['list', '--json', '1'])
    out_json = json.loads(sp.stdout)
    count = sum(target.get("current", False) for target in out_json)
    # Make sure there is only 1 "current" version
    assert count == 1
    curr_target = next(target for target in out_json if target.get("current", False))
    return curr_target["version"]

def invoke_aklite(options):
    if offline:
        options = options + [ "--src-dir", os.path.abspath("./offline-bundles/unified/") ]
    logger.info("Running `" + " ".join([aklite_path] + options) + "`")
    return subprocess.run([aklite_path] + options, capture_output=True)

def write_settings(apps=None, prune=True):
    logger.info(f"Updating settings. {apps=}")
    callback_file = "/var/sota/callback.sh"

    callback_content = \
"""#!/bin/sh
echo { \\"MESSAGE\\": \\"$MESSAGE\\", \\"CURRENT_TARGET\\": \\"$CURRENT_TARGET\\", \\"CURRENT_TARGET_NAME\\": \\"$CURRENT_TARGET_NAME\\", \\"INSTALL_TARGET_NAME\\": \\"$INSTALL_TARGET_NAME\\", \\"RESULT\\": \\"$RESULT\\" } >> /var/sota/callback_log.txt
"""
    with open(callback_file, "w") as f:
        f.write(callback_content)
    st = os.stat(callback_file)
    os.chmod(callback_file, st.st_mode | stat.S_IEXEC)

    content = \
f"""
[pacman]
tags = "main"
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
        sota_toml_content = sota_toml_content.replace("[pacman]", '[pacman]\ncallback_program = "/var/sota/callback.sh"')
        with open("/var/sota/sota.toml", "w") as f:
            f.write(sota_toml_content)

def get_all_current_apps():
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

def check_running_apps(expected_apps=None):
    # if no apps list is specified, all apps should be running
    if expected_apps is None:
        expected_apps = get_all_current_apps()
    logger.info(f"Verifying running apps. {expected_apps=}")
    running_apps = get_running_apps()
    assert set(expected_apps) == set(running_apps)

def cleanup_tuf_metadata():
    os.system("""sqlite3 /var/sota/sql.db  "delete from meta where meta_type <> 0 or version >= 3;" ".exit" """)

def cleanup_installed_data():
    os.system("""sqlite3 /var/sota/sql.db  "delete from installed_versions;" ".exit" """)

def install_with_separate_steps(version, requires_reboot=False, install_rollback=False, run_rollback=False, explicit_version=True):
    cp = invoke_aklite(['check', '--json', '1'])
    assert cp.returncode == ReturnCodes.CheckinUpdateNewVersion
    verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

    if install_rollback or run_rollback:
        final_version = aklite_current_version()
    else:
        final_version = version

    if explicit_version:
        cp = invoke_aklite(['pull', str(version)])
    else:
        cp = invoke_aklite(['pull'])
    assert cp.returncode == ReturnCodes.Ok
    verify_callback([("download-pre", ""), ("download-post", "OK")])
    verify_events(version, {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
        })

    # cp = invoke_aklite(['install', str(get_target_version(Targets.BrokenBuild))]) # not existing target
    # assert cp.returncode == ReturnCodes.TufTargetNotFound
    # verify_callback([])

    # version = 159
    # cp = invoke_aklite(['install', str(version)]) # not downloaded target
    # assert cp.returncode == ReturnCodes.InstallAppPullFailure
    # verify_callback([])

    if explicit_version:
        cp = invoke_aklite(['install', str(version)]) # OK
    else:
        cp = invoke_aklite(['install']) # OK
    if install_rollback:
        assert cp.returncode == ReturnCodes.InstallRollbackOk
        verify_callback([("install-pre", ""), ("install-post", "FAILED"), ("install-pre", ""), ("install-post", "OK")])
        verify_events(version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', False),
        }, True)
        verify_events(final_version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', True),
        }, False)

    elif requires_reboot:
        assert cp.returncode == ReturnCodes.InstallNeedsReboot
        verify_callback([("install-pre", ""), ("install-post", "NEEDS_COMPLETION")])
        sys_reboot()
        verify_events(version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        cp = invoke_aklite(['run'])
        verify_callback([("install-final-pre", ""), ("install-post", "OK")])
        verify_events(version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
                ('EcuInstallationCompleted', True),
            })
    else:
        if run_rollback:
            assert cp.returncode == ReturnCodes.InstallRollbackOk
            verify_callback([
                ("install-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', False),
            }, True)

            verify_events(final_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            }, False)

        else:
            assert cp.returncode == ReturnCodes.Ok
            verify_callback([("install-pre", ""), ("install-post", "OK")])
            verify_events(version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })

    assert aklite_current_version() == final_version
    if not explicit_version:
        # Make sure we would not try a new install, after trying to install the latest one
        cp = invoke_aklite(['check', '--json', '1'])
        assert cp.returncode == ReturnCodes.Ok
        verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

def install_with_single_step(version, requires_reboot=False, install_rollback=False, run_rollback=False, explicit_version=True):
    if install_rollback or run_rollback:
        final_version = aklite_current_version()
    else:
        final_version = version

    if explicit_version:
        cp = invoke_aklite(['update', str(version)])
    else:
        cp = invoke_aklite(['update'])

    if install_rollback:
        assert cp.returncode == ReturnCodes.InstallRollbackOk
        verify_callback([
            ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
            ("download-pre", ""), ("download-post", "OK"),
            ("install-pre", ""), ("install-post", "FAILED"),
            ("install-pre", ""), ("install-post", "OK")])
        verify_events(version, {
            ('EcuDownloadStarted', None),
            ('EcuDownloadCompleted', True),
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', False),
        }, True)
        verify_events(final_version, {
            ('EcuInstallationStarted', None),
            ('EcuInstallationCompleted', True),
        }, False)

    elif requires_reboot:
        assert cp.returncode == ReturnCodes.InstallNeedsReboot
        verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "NEEDS_COMPLETION"),
                ])
        sys_reboot()
        verify_events(version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
            })
        cp = invoke_aklite(['run'])
        # TODO: handle run_rollback
        verify_callback([("install-final-pre", ""), ("install-post", "OK")])
        verify_events(version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationApplied', None),
                ('EcuInstallationCompleted', True),
            })
    else:
        if run_rollback:
            assert cp.returncode == ReturnCodes.InstallRollbackOk
            verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "FAILED"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', False),
            }, True)

            verify_events(final_version, {
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            }, False)

        else:
            assert cp.returncode == ReturnCodes.Ok
            verify_callback([
                ("check-for-update-pre", ""), ("check-for-update-post", "OK"),
                ("download-pre", ""), ("download-post", "OK"),
                ("install-pre", ""), ("install-post", "OK")
                ])
            verify_events(version, {
                ('EcuDownloadStarted', None),
                ('EcuDownloadCompleted', True),
                ('EcuInstallationStarted', None),
                ('EcuInstallationCompleted', True),
            })
    assert aklite_current_version() == final_version

    if not explicit_version:
        # Make sure we would not try a new install, after trying to install the latest one
        cp = invoke_aklite(['check', '--json', '1'])
        assert cp.returncode == ReturnCodes.Ok
        verify_callback([("check-for-update-pre", ""), ("check-for-update-post", "OK")])

def install_version(version, requires_reboot=False, install_rollback=False, run_rollback=False, explicit_version=True):
    if single_step:
        install_with_single_step(version, requires_reboot, install_rollback, run_rollback, explicit_version)
    else:
        install_with_separate_steps(version, requires_reboot, install_rollback, run_rollback, explicit_version)

def restore_system_state():
    # Get to the starting point
    logger.info(f"Restoring base environment. Offline={offline}, SingleStep={single_step}, Prune={prune}...")
    write_settings()
    sys_reboot()
    cp = invoke_aklite(['run'])
    version = get_target_version(Targets.First)
    cleanup_installed_data()
    cp = invoke_aklite(['update', str(version)])
    print(cp.stdout)
    sys_reboot()
    cp = invoke_aklite(['run'])
    assert aklite_current_version() == version
    clear_callbacks_log()
    cleanup_tuf_metadata()

    logger.info("Making sure there are no targets in current DB...")
    cp = invoke_aklite(['list', '--json', '1'])
    assert cp.returncode == ReturnCodes.CheckinSecurityError

# Incremental install order
install_sequence_incremental = [
    Targets.BrokenOstree,
    Targets.WorkingOstree,
    Targets.AddFirstApp,
    Targets.AddMoreApps,
    Targets.BreakApp,
    Targets.UpdateBrokenApp,
    Targets.BrokenBuild,
    Targets.FixApp,
    Targets.UpdateWorkingApp,
    Targets.UpdateOstreeWithApps,
    Targets.BrokenOstreeWithApps,
]

def run_test_sequence_incremental():
    restore_system_state()
    apps = None # All apps, for now
    prev_ostree_image_version = 1
    for target_version in install_sequence_incremental:
        target = all_targets[target_version]
        if target.build_error: # skip this one for now
            continue

        version = get_target_version(target.version_offset)
        logger.info(f"Updating to {version} {target}. SingleStep={single_step}, Offline={offline}")
        requires_reboot = target.ostree_image_version != prev_ostree_image_version
        write_settings(apps, prune)
        install_version(version, requires_reboot, target.install_rollback, target.run_rollback)
        check_running_apps(apps)
        if not target.install_rollback and not target.run_rollback:
            prev_ostree_image_version = target.ostree_image_version

def run_test_sequence_update_to_latest():
    restore_system_state()
    apps = None # All apps, for now
    prev_ostree_image_version = 1

    last_target = all_targets[Targets.BrokenOstreeWithApps]
    target = last_target

    # Try to install latest version, which will lead to a rollback
    write_settings(apps, prune)
    version = get_target_version(target.version_offset)
    requires_reboot = target.ostree_image_version != prev_ostree_image_version
    logger.info(f"Updating to latest target {version} {target} {single_step=} {offline=}")
    install_version(version, requires_reboot, target.install_rollback, target.run_rollback, False)
    check_running_apps(apps)

def run_test_sequence_apps_selection():
    restore_system_state()
    apps = None # All apps, for now
    prev_ostree_image_version = 1
    target_version = Targets.AddMoreApps
    target = all_targets[target_version]

    version = get_target_version(target.version_offset)
    logger.info(f"Updating to target {version} {target}. {single_step=} {offline=}")
    requires_reboot = target.ostree_image_version != prev_ostree_image_version
    write_settings(apps, prune)
    install_version(version, requires_reboot, target.install_rollback, target.run_rollback)
    check_running_apps(apps)
    prev_ostree_image_version = target.version_offset

    apps = ["shellhttpd"]
    write_settings(apps, prune)
    logger.info(f"Forcing apps sync for target {version} {target}. {single_step=} {offline=}")
    cp = invoke_aklite(['update', str(version)])
    assert cp.returncode == ReturnCodes.Ok
    check_running_apps(apps)

    apps = all_apps
    write_settings(apps, prune)
    logger.info(f"Forcing apps sync for target {version} {target}. {single_step=} {offline=}")
    cp = invoke_aklite(['update', str(version)])
    assert cp.returncode == ReturnCodes.Ok
    check_running_apps(apps)

    if not target.install_rollback and not target.run_rollback:
        prev_ostree_image_version = target.ostree_image_version

def test_apps_selection():
    run_test_sequence_apps_selection()

@pytest.mark.parametrize('offline_', [True, False])
@pytest.mark.parametrize('single_step_', [True, False])
def test_incremental_updates(offline_, single_step_):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    run_test_sequence_incremental()

@pytest.mark.parametrize('offline_', [True, False])
@pytest.mark.parametrize('single_step_', [True, False])
def test_update_to_latest(offline_, single_step_):
    global offline, single_step
    offline = offline_
    single_step = single_step_
    run_test_sequence_update_to_latest()
