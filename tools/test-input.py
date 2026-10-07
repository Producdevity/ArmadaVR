#!/usr/bin/env python3
"""Exercise Monado's virtual controllers through the real OpenXR action API."""
import argparse
import json
import math
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time


HEADER = b"mndrmt3\0"
PACKET_BYTES = 376


def receive(connection):
    data = bytearray()
    while len(data) < PACKET_BYTES:
        chunk = connection.recv(PACKET_BYTES - len(data))
        if not chunk:
            raise ConnectionError("Monado closed the virtual-controller connection")
        data.extend(chunk)
    if data[:8] != HEADER:
        raise ValueError("Unsupported Monado remote protocol")
    return data


def controls(initial, elapsed):
    # Native wire layout in Monado 01c1f6b's drivers/remote/r_interface.h.
    packet = bytearray(initial)
    struct.pack_into("<f", packet, 124, 1.6 + 0.05 * math.sin(elapsed))
    for hand, offset in enumerate((136, 256)):
        pressed = 1 <= elapsed < 2.5 if hand == 0 else 2.5 <= elapsed < 4
        struct.pack_into("<f", packet, offset + 16, (-0.2 if hand == 0 else 0.2) +
                         0.05 * math.sin(elapsed))
        struct.pack_into("<f", packet, offset + 72, 0.75 if pressed else 0)
        packet[offset + 105] = not 5 <= elapsed < 6
        packet[offset + 112] = pressed
    return packet


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def check_captures(output):
    expected = {"baseline": ("red", "blue"), "left": ("yellow", "blue"),
                "right": ("red", "cyan"), "released": ("red", "blue")}
    results = {}
    for name, required in expected.items():
        data = subprocess.run(["magick", str(output / f"{name}.png"), "-depth", "8", "rgb:-"],
                              check=True, capture_output=True, timeout=10).stdout
        if len(data) != 1280 * 800 * 3:
            raise ValueError("Unexpected rendered input capture dimensions")
        counts = dict.fromkeys(("red", "blue", "yellow", "cyan"), 0)
        positions = dict.fromkeys(counts, 0)
        for pixel, (red, green, blue) in enumerate(zip(data[0::3], data[1::3], data[2::3])):
            colors = {"red": red > 140 and green < 100 and blue < 100,
                      "blue": blue > 140 and red < 100 and green < 100,
                      "yellow": red > 140 and green > 140 and blue < 100,
                      "cyan": blue > 140 and green > 140 and red < 100}
            for color, matches in colors.items():
                if matches:
                    counts[color] += 1
                    positions[color] += pixel % 1280
        results[name] = counts
        if any(counts[color] < 1000 for color in required):
            raise ValueError(f"Controller state was not visible in {name}: {counts}")
        if any(counts[color] >= 1000 for color in counts if color not in required):
            raise ValueError(f"Unexpected controller state remained visible in {name}: {counts}")
        if positions[required[0]] / counts[required[0]] >= positions[required[1]] / counts[required[1]]:
            raise ValueError(f"Left and right eye output is reversed in {name}")
    (output / "captures.json").write_text(json.dumps(results, indent=2) + "\n")
    print("ARMADA_VR_RENDERED_INPUT_PASS (captured eye colors follow both real action states)")


def run(output, probe, windows=False, render=False):
    output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix="armada-input-") as temporary:
        root = Path(temporary)
        config = root / "monado"
        config.mkdir()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            port = reservation.getsockname()[1]
        (config / "config_v0.json").write_text(json.dumps({
            "active": "remote", "remote": {"version": 0, "port": port, "view_count": 2}}))
        # A non-socket D-Bus path prevents portal mounts and passes Runtime 4's bind check.
        env = os.environ | {"XDG_CONFIG_HOME": str(root), "XDG_RUNTIME_DIR": str(root),
                            "DBUS_SESSION_BUS_ADDRESS": "unix:path=/dev/null",
                            "P_OVERRIDE_ACTIVE_CONFIG": "remote", "XRT_NO_STDIN": "1",
                            "XRT_COMPOSITOR_NULL": "1", "REMOTE_LOG": "info",
                            "XR_RUNTIME_JSON": "/usr/share/openxr/1/openxr_monado.json"}
        if render:
            if not env.get("DISPLAY"):
                raise ValueError("Rendered input requires a display; run under xvfb-run with a 1280x800x24 screen")
            env.pop("XRT_COMPOSITOR_NULL")
            env.update({"XRT_COMPOSITOR_FORCE_XCB": "1", "XRT_COMPOSITOR_SCALE_PERCENTAGE": "40",
                        "LP_NUM_THREADS": "2",
                        "VK_DRIVER_FILES": os.environ.get("VK_DRIVER_FILES", "/usr/share/vulkan/icd.d/lvp_icd.aarch64.json")})
        result_file = output / ("probe.log" if windows else "openxr.log")
        if windows:
            env["ARMADA_VR_PROBE_OUTPUT"] = "Z:" + str(result_file.resolve())
        service = application = None
        try:
            with (output / "monado.log").open("w") as log, (output / "openxr.log").open("w") as app_log:
                service = subprocess.Popen(["monado-service"], env=env, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 10
                while True:
                    if service.poll() is not None:
                        raise RuntimeError("Virtual Monado service exited; see monado.log")
                    try:
                        connection = socket.create_connection(("127.0.0.1", port), timeout=1)
                        break
                    except ConnectionRefusedError:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Virtual controller service did not listen")
                        time.sleep(0.05)
                with connection:
                    receive(connection)
                    initial = receive(connection)
                    while not (root / "monado_comp_ipc").is_socket():
                        if service.poll() is not None or time.monotonic() >= deadline:
                            raise TimeoutError("Virtual OpenXR service did not become ready")
                        time.sleep(0.05)
                    command = [str(probe), "--remote-input"]
                    if render:
                        command.append("--render")
                    if windows:
                        command.insert(0, "/usr/local/bin/run-proton-openxr")
                    application = subprocess.Popen(command, env=env,
                                                   stdout=app_log, stderr=subprocess.STDOUT)
                    started = time.monotonic()
                    input_started = None
                    captures = iter(((0.5, "baseline"), (1.8, "left"), (3.3, "right"), (4.6, "released")))
                    pending_capture = next(captures)
                    while application.poll() is None:
                        elapsed = time.monotonic() - started
                        if elapsed > (90 if windows else 50 if render else 25):
                            raise TimeoutError("OpenXR action probe exceeded its deadline")
                        if (input_started is None and result_file.exists() and
                                "ARMADA_VR_INPUT_READY" in result_file.read_text()):
                            input_started = time.monotonic()
                        connection.sendall(controls(initial, 0 if input_started is None else
                                                    time.monotonic() - input_started))
                        if render and input_started is not None and pending_capture is not None:
                            capture_time, name = pending_capture
                            if time.monotonic() - input_started >= capture_time:
                                subprocess.run(["import", "-window", "root", str(output / f"{name}.png")],
                                               env=env, check=True, timeout=5)
                                pending_capture = next(captures, None)
                        time.sleep(0.02)
                    if application.returncode:
                        raise RuntimeError("OpenXR action probe failed; see openxr.log")
            result = result_file.read_text()
            if "ARMADA_VR_INPUT_PASS" not in result:
                raise RuntimeError("Missing OpenXR action acceptance result")
            print(result, end="")
            if render:
                if "ARMADA_VR_RENDERED_INPUT_SUBMITTED" not in result:
                    raise RuntimeError("Missing rendered input submission result")
                check_captures(output)
        finally:
            stop(application)
            stop(service)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--windows", action="store_true", help="Test the Windows probe through Proton and its OpenXR loader")
    parser.add_argument("--probe", type=Path)
    parser.add_argument("--render", action="store_true", help="Require rendered stereo output to follow both controller actions")
    args = parser.parse_args()
    try:
        probe = args.probe or Path("/opt/armada-vr/windows/xr-probe.exe" if args.windows else "/usr/local/bin/xr-probe")
        run(args.output, probe, args.windows, args.render)
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
