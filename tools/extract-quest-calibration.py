#!/usr/bin/env python3
"""Preserve and validate unit calibration from an offline Quest sensor-service dump."""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import stat


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value):
    require(type(value) in (int, float) and math.isfinite(value), "Expected a finite number")
    return value


def vector(value, length):
    require(isinstance(value, list) and len(value) == length, "Invalid vector length")
    return [number(item) for item in value]


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate JSON key: " + key)
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError("Non-finite JSON constant: " + value)


DECODER = json.JSONDecoder(object_pairs_hook=unique_object, parse_constant=invalid_constant,
                           parse_float=lambda value: number(float(value)))


def determinant(matrix):
    a, b, c, d, e, f, g, h, i = matrix
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)


def rigid_transform(value):
    matrix = vector(value, 16)
    require(all(abs(a - b) < 1e-6 for a, b in zip(matrix[12:], [0, 0, 0, 1])),
            "Invalid homogeneous transform row")
    rows = [matrix[start:start + 3] for start in (0, 4, 8)]
    for i in range(3):
        for j in range(3):
            dot = sum(a * b for a, b in zip(rows[i], rows[j]))
            require(abs(dot - (1 if i == j else 0)) < 1e-4, "Non-rigid rotation")
    require(abs(determinant(sum(rows, [])) - 1) < 1e-4, "Reflected rotation")


def imu_model(model, time_field):
    require(model["Model"] == "Linear", "Unsupported IMU model")
    matrix = vector(model["RectificationMatrix"], 9)
    require(abs(determinant(matrix)) > 1e-8, "Singular IMU rectification")
    number(model[time_field])
    offset = model["Offset"]
    if offset["Model"] == "Constant":
        vector(offset["ConstantOffset"], 3)
    elif offset["Model"] == "TemperaturePiecewiseLinear":
        require(offset["CoordinateSystem"] == "Raw", "Unsupported offset coordinate system")
        require(offset["DataDescription"] == "temperature, offset.x, offset.y, offset.z",
                "Unsupported temperature row layout")
        pairs = offset["TemperatureOffsetPairs"]
        require(isinstance(pairs, list) and 2 <= len(pairs) <= 1024, "Invalid temperature table")
        temperatures = [vector(row, 4)[0] for row in pairs]
        require(all(a < b for a, b in zip(temperatures, temperatures[1:])),
                "Temperature rows must be strictly increasing")
    else:
        raise ValueError("Unsupported IMU offset model")


def calibration(text, serial, online):
    data = DECODER.decode(text)
    require(data["FileFormat"]["Version"] == "1", "Unsupported calibration version")
    device = data["Device"]
    require(device["SerialNumber"] == serial and device["DeviceType"] == "Eureka",
            "Calibration belongs to another headset")
    imu = data["ImuCalibration"]
    require(imu["Id"] == "imu0" and imu["SensorType"] == "ICM45688", "Unsupported IMU identity")
    rigid_transform(imu["DeviceFromImu"])
    for sensor, field in (("Accelerometer", "DtAccelRef"), ("Gyroscope", "DtGyroRef")):
        imu_model(imu[sensor], field)
    cameras = data.get("CameraCalibration", [])
    require(isinstance(cameras, list) and len(cameras) <= 16, "Invalid camera list")
    require(not online or len(cameras) > 0, "Online camera calibration is missing")
    identifiers = set()
    for camera in cameras:
        ident = camera["Id"]
        require(isinstance(ident, str) and ident and ident not in identifiers, "Duplicate or invalid camera ID")
        identifiers.add(ident)
        require(camera["SensorType"] in ("OG01A", "OV7251", "IMX471"), "Unsupported camera sensor")
        width, height = vector(camera["ImageSize"], 2)
        require(type(width) is int and type(height) is int and 0 < width <= 16384 and 0 < height <= 16384,
                "Invalid camera dimensions")
        rigid_transform(camera["DeviceFromCamera"])
        projection = camera["Projection"]
        require(projection["Model"] == "PinholeSymmetric", "Unsupported projection")
        require(vector(projection["Coefficients"], 3)[0] > 0, "Invalid focal length")
        distortion = camera["Distortion"]
        require(distortion["Model"] == "Fisheye62", "Unsupported distortion")
        vector(distortion["Coefficients"], 8)
        shutter = camera["Shutter"]
        require(shutter["Type"] in ("Global", "Rolling"), "Unsupported shutter")
        if shutter["Type"] == "Rolling":
            require(number(shutter["ReadoutTimeSec"]) > 0, "Invalid rolling shutter duration")
        if "UnprojectedFovUnitPlane" in camera:
            plane = camera["UnprojectedFovUnitPlane"]
            require(isinstance(plane["x"], list) and 3 <= len(plane["x"]) <= 1024, "Invalid FOV boundary")
            vector(plane["x"], len(plane["x"]))
            vector(plane["y"], len(plane["x"]))
    return {"imu_type": imu["SensorType"], "camera_count": len(cameras),
            "offset_models": {name: imu[name]["Offset"]["Model"] for name in ("Accelerometer", "Gyroscope")}}


def extract(capture, serial, output):
    require(bool(serial.strip()), "An expected unit serial is required")
    require(not capture.is_symlink() and stat.S_ISREG(capture.stat().st_mode), "Expected a regular capture file")
    with os.fdopen(os.open(capture, os.O_RDONLY | os.O_NOFOLLOW), "rb") as stream:
        require(stat.S_ISREG(os.fstat(stream.fileno()).st_mode), "Expected a regular capture file")
        raw = stream.read(2 * 1024 * 1024 + 1)
    require(len(raw) <= 2 * 1024 * 1024, "Capture exceeds 2 MiB")
    sensors = []
    for line in raw.decode("utf-8").splitlines():
        prefix, separator, content = line.partition(") ")
        if not separator or not prefix.startswith("0x") or not content.startswith("{"):
            continue
        sensor, end = DECODER.raw_decode(content)
        require(content[end:].startswith(" | "), "Invalid sensor descriptor boundary")
        if sensor.get("aname") == "Syncboss IMU Android Sensor":
            sensors.append(sensor)
    require(len(sensors) == 1, "Expected exactly one Syncboss IMU descriptor")
    sensor = sensors[0]
    require(sensor["info"]["model"] == "ICM45688", "Unsupported descriptor IMU")
    require(number(sensor["dataRate"]) > 0, "Invalid advertised data rate")
    files = {}
    for kind in ("factory", "online"):
        text = sensor["calibration"][kind]
        require(isinstance(text, str), "Expected an embedded calibration JSON string")
        summary = calibration(text, serial, kind == "online")
        payload = text.encode("utf-8")
        files[kind] = (payload, {**summary, "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)})
    report = {"source": "offline dumpsys sensorservice capture", "source_sha256": hashlib.sha256(raw).hexdigest(),
              "device_type": "Eureka", "serial": serial, "advertised_data_rate_hz": sensor["dataRate"],
              "files": {kind + ".json": info for kind, (_, info) in files.items()},
              "calibration_applied": False, "physical_tracking_verified": False,
              "capture_authentication": "not established by this offline parser"}
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    for name, payload in [(kind + ".json", data) for kind, (data, _) in files.items()] + [
        ("manifest.json", (json.dumps(report, indent=2) + "\n").encode())
    ]:
        with os.fdopen(os.open(output / name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--serial", required=True, help="Expected headset serial; never inferred from the capture")
    parser.add_argument("--output", required=True, type=Path, help="New private output directory")
    args = parser.parse_args()
    try:
        report = extract(args.capture, args.serial, args.output)
    except (OSError, ValueError, KeyError, TypeError, OverflowError, RecursionError) as error:
        parser.exit(1, str(error) + "\n")
    print(json.dumps({"status": "preserved", "files": report["files"], "physical_tracking_verified": False}, indent=2))


if __name__ == "__main__":
    main()
