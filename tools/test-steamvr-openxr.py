#!/usr/bin/env python3
"""Run the OpenXR presentation sample in this user's active virtual SteamVR session."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def run(bundle, output):
    if os.getuid() == 0:
        raise ValueError("Run as the desktop user, not root")
    report = json.loads((bundle / "build.json").read_text())
    if report.get("target") != "steamvr-openxr-x86_64":
        raise ValueError("Expected the OpenXR test bundle")
    if not {"steamvr-openxr", "lib/libopenxr_loader.so.1"} <= report.get("sha256", {}).keys():
        raise ValueError("Missing executable or loader checksum")
    for name, expected in report["sha256"].items():
        relative = Path(name)
        path = bundle / relative
        if relative.is_absolute() or ".." in relative.parts or path.is_symlink() or not path.is_file():
            raise ValueError(f"Invalid bundle path: {name}")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Bundle checksum mismatch: {name}")
    processes = []
    for path in Path("/proc").glob("[0-9]*/comm"):
        try:
            if path.stat().st_uid == os.getuid() and path.read_text().strip() == "vrcompositor":
                processes.append(path.parent)
        except FileNotFoundError:
            continue
    if len(processes) != 1:
        raise ValueError("Expected one active SteamVR compositor owned by this user")
    original = dict(entry.decode().split("=", 1) for entry in
                    (processes[0] / "environ").read_bytes().split(b"\0") if b"=" in entry)
    names = ("HOME", "USER", "PATH", "DISPLAY", "XAUTHORITY", "XDG_RUNTIME_DIR", "FEX_APP_CONFIG_LOCATION",
             "FEX_PORTABLE", "FEX_ENV", "VK_DRIVER_FILES", "LVP_DRM_SYNC", "LP_NUM_THREADS", "LD_LIBRARY_PATH",
             "VRCOMPOSITOR_LD_LIBRARY_PATH", "VR_PATHREG_OVERRIDE", "VR_OVERRIDE", "VR_CONFIG_PATH", "VR_LOG_PATH",
             "STEAMVR_TOOLSDIR", "STEAMVR_VRENV", "MESA_VK_WSI_SW_PRESENT")
    env = {key: original[key] for key in names if key in original}
    expected_registry = Path.home() / ".local/state/armada-vr/steamvr-virtual/openvrpaths.vrpath"
    if Path(env.get("VR_PATHREG_OVERRIDE", "")).resolve() != expected_registry.resolve():
        raise ValueError("Refusing a session outside the private virtual-headset registry")
    registry = json.loads(expected_registry.read_text())
    settings = json.loads((Path(registry["config"][0]) / "steamvr.vrsettings").read_text())
    if settings.get("steamvr", {}).get("forcedDriver") != "null" or not settings.get("driver_null", {}).get("enable"):
        raise ValueError("This test requires the virtual null headset")
    runtime = Path(registry["runtime"][0])
    fex = (processes[0] / "exe").resolve(strict=True)
    if fex.name != "FEX" or not fex.is_file():
        raise ValueError("Expected the active compositor's FEX interpreter")
    env["XR_RUNTIME_JSON"] = str(runtime / "steamxr_linux64.json")
    env["XR_LOADER_DEBUG"] = "warn"
    env["LD_LIBRARY_PATH"] = str(bundle / "lib") + ":" + env.get("LD_LIBRARY_PATH", "")
    output.mkdir(parents=True, exist_ok=False)
    command = [str(fex), str(bundle / "steamvr-openxr"), str(runtime / "bin/linux64/libopenvr_api.so")]
    result = {"target": "virtual-steamvr-openxr", "status": "started", "physical_headset_tested": False,
              "bundle_sha256": hashlib.sha256((bundle / "build.json").read_bytes()).hexdigest()}
    try:
        with (output / "probe.log").open("wb") as log:
            subprocess.run(command, env=env, cwd=runtime / "bin/linux64", timeout=100, check=True,
                           stdout=log, stderr=subprocess.STDOUT, restore_signals=False)
        if "STEAMVR_OPENXR_PRESENT_PASS" not in (output / "probe.log").read_text(errors="replace"):
            raise RuntimeError("Missing actual OpenXR presentation acceptance")
        result["status"] = "passed"
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"OpenXR through SteamVR passed: {output}; verify display captures separately")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    try:
        run(args.bundle.resolve(), args.output.resolve())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
