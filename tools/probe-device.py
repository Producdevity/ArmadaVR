#!/usr/bin/env python3
"""Collect an explicit headset's non-root bring-up inventory over ADB."""
import argparse
import datetime
import json
import re
import subprocess
from pathlib import Path

PROPERTIES = (
    "ro.product.manufacturer", "ro.product.model", "ro.product.device",
    "ro.product.board", "ro.board.platform", "ro.soc.model",
    "ro.product.cpu.abilist", "ro.build.fingerprint", "ro.build.type",
    "ro.build.version.release", "ro.build.version.incremental",
    "ro.build.version.security_patch", "ro.boot.hardware",
    "ro.boot.slot_suffix", "ro.boot.flash.locked", "ro.boot.vbmeta.device_state",
    "ro.boot.verifiedbootstate", "ro.oem_unlock_supported",
    "ro.boot.bootloader", "ro.boot.hardware.sku", "ro.boot.hwrev",
    "ro.boot.dtbo_idx", "ro.boot.veritymode", "ro.boot.vbmeta.avb_version",
    "ro.product.vendor.device", "ro.vendor.build.fingerprint",
    "ro.pico.tag", "ro.secure.boot.tag", "ro.oem.state",
)
QUERIES = {
    "kernel": "uname -a",
    "cpu": "cat /sys/devices/system/cpu/present; cat /sys/devices/system/cpu/online",
    "memory": "cat /proc/meminfo",
    "device_tree": "cat /sys/firmware/devicetree/base/compatible",
    "block_devices": "cat /proc/partitions; ls -l /dev/block/by-name /dev/block/bootdevice/by-name",
    "graphics": "ls -l /dev/dri /dev/kgsl-3d0 /sys/class/drm",
    "input_devices": "cat /proc/bus/input/devices",
    "sensors": "ls /sys/bus/iio/devices /sys/class/thermal",
    "storage": "df -k /data",
    "shell_identity": "id",
    "selinux": "getenforce",
    "root_binary": "command -v su",
    "soc_identity": "cat /sys/devices/soc0/soc_id /sys/devices/soc0/revision /sys/devices/soc0/machine",
    "board_ids": "od -An -tx1 /sys/firmware/devicetree/base/qcom,board-id; od -An -tx1 /sys/firmware/devicetree/base/qcom,msm-id",
    "gpu_model": "cat /sys/class/kgsl/kgsl-3d0/gpu_model",
    "display_status": "cat /sys/class/drm/card0-DSI-1/status",
    "display_modes": "cat /sys/class/drm/card0-DSI-1/modes",
    "loaded_modules": "cat /proc/modules",
    "vendor_modules": "ls /vendor/lib/modules",
    "odm_modules": "ls /odm/lib/modules",
    "kernel_config": "zcat /proc/config.gz",
}


def observations(properties, queries):
    def value(records, key):
        record = records.get(key, {})
        text = record.get("stdout", "").strip()
        return text if record.get("returncode") == 0 and text else None

    locked = value(properties, "ro.boot.flash.locked")
    state = value(properties, "ro.boot.vbmeta.device_state")
    claims = {candidate for candidate in (
        {"0": "unlocked", "1": "locked"}.get(locked),
        {"unlocked": "unlocked", "locked": "locked"}.get(state),
    ) if candidate is not None}
    lock_state = "conflicting" if len(claims) > 1 else next(iter(claims), "unknown")
    identity = value(queries, "shell_identity")
    uid = re.search(r"(?:^|\s)uid=(\d+)(?:\(|\s|$)", identity or "")
    return {
        "firmware_incremental": value(properties, "ro.build.version.incremental"),
        "firmware_fingerprint": value(properties, "ro.build.fingerprint"),
        "security_patch": value(properties, "ro.build.version.security_patch"),
        "bootloader_reported_state": lock_state,
        "verified_boot_reported_state": value(properties, "ro.boot.verifiedbootstate"),
        "adb_shell_uid": int(uid.group(1)) if uid else None,
        "su_path": value(queries, "root_binary"),
        "selinux_reported_state": value(queries, "selinux"),
        "selected_dtbo_indices": value(properties, "ro.boot.dtbo_idx"),
        "pico_firmware_tag": value(properties, "ro.pico.tag"),
        "pico_secure_boot_tag": value(properties, "ro.secure.boot.tag"),
        "pico_oem_state": value(properties, "ro.oem.state"),
        "limitations": [
            "Android properties may be absent or spoofed; they do not prove unsigned boot or recovery.",
            "A non-root ADB shell does not rule out root through another service; su is not invoked.",
            "No exploit compatibility is inferred from version ordering or security-patch dates.",
        ],
    }


def invoke(adb, serial, args):
    try:
        result = subprocess.run([adb, "-s", serial, *args], capture_output=True,
                                text=True, timeout=15, check=False)
        return {"returncode": result.returncode, "stdout": result.stdout.strip(),
                "stderr": result.stderr.strip()}
    except subprocess.TimeoutExpired:
        return {"returncode": None, "stdout": "", "stderr": "Timed out after 15 seconds"}


def collect(adb, serial):
    state = invoke(adb, serial, ["get-state"])
    if state["returncode"] != 0 or state["stdout"] != "device":
        detail = state.get("stderr") or state.get("stdout") or "No device state returned"
        raise RuntimeError(f"Could not reach the selected authorized ADB device: {detail[:512]}")
    props = {key: invoke(adb, serial, ["shell", "getprop", key]) for key in PROPERTIES}
    identity = " ".join(props[key]["stdout"].lower() for key in (
        "ro.product.manufacturer", "ro.product.model", "ro.product.device"))
    if not any(name in identity for name in ("quest", "eureka", "panther", "pico", "neo3")):
        raise RuntimeError("The selected device does not identify as a Quest or Pico headset")
    queries = {name: invoke(adb, serial, ["shell", command]) for name, command in QUERIES.items()}
    return {
        "schema_version": 2,
        "collected_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "mode": "adb-shell-read-only",
        "properties": props,
        "queries": queries,
        "observations": observations(props, queries),
        "interpretation": {
            "bootloader_unlock_proven": False,
            "recovery_restore_proven": False,
            "physical_linux_boot_proven": False,
            "note": "Properties are observations, not proof that an unsigned image can boot or be restored.",
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="Exact ADB serial; never selects a default device")
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--output", type=Path, required=True, help="New JSON report path")
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output already exists; choose a new report path")
    try:
        report = collect(args.adb, args.serial)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("x") as stream:
            json.dump(report, stream, indent=2)
            stream.write("\n")
    except (OSError, RuntimeError) as error:
        parser.exit(1, f"{error}\n")
    print(f"Read-only inventory saved to {args.output}")


if __name__ == "__main__":
    main()
