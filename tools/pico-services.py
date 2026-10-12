"""Install the pinned Pico QRTR and service-map runtime into an offline root."""
import hashlib
from pathlib import Path
import re
import shutil
import stat
import subprocess


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ("usr/bin/qrtr-ns", "usr/bin/qrtr-lookup", "usr/lib64/libqrtr.so.1.2")
LIBRARY_LINK = "usr/lib64/libqrtr.so.1"
LIBRARY_TARGET = "libqrtr.so.1.2"
PROVENANCE = {
    "usr/share/armada-qcom-services/sources.json": "profiles/qcom-services.json",
    "usr/share/armada-qcom-services/0001-check-qmi-packet-bounds.patch":
        "patches/qrtr/0001-check-qmi-packet-bounds.patch",
}
LICENSES = ("usr/share/licenses/armada-qcom-services/qrtr",
            "usr/share/licenses/armada-qcom-services/qrtr-ns")
MAPPER = "usr/bin/pd-mapper"
MAP_CHECKSUMS = "etc/armada-vr/pico-service-maps.sha256"
MAP_DOMAINS = {"adspr.jsn": ("adsp", "root_pd", 74), "adspua.jsn": ("adsp", "audio_pd", 74),
               "cdspr.jsn": ("cdsp", "root_pd", 76), "slpir.jsn": ("slpi", "root_pd", 90),
               "slpius.jsn": ("slpi", "sensor_pd", 90)}
MAPPER_PROVENANCE = {"usr/share/armada-qcom-services/" + name: "patches/pd-mapper/" + name for name in (
    "0001-report-actual-firmware-path.patch", "0002-explicit-service-maps.patch",
    "0003-check-firmware-attribute-length.patch")}
MAPPER_LICENSE = "usr/share/licenses/armada-qcom-services/pd-mapper"


def service_maps(manifest):
    if manifest is None:
        return None
    if manifest.get("component") == "pico-neo3-gpu" and "dsp" not in manifest:
        return None
    if (manifest.get("component") != "pico-neo3-firmware" or manifest.get("status") != "prepared" or
            manifest.get("schema_version") != 1):
        raise ValueError("Requires verified Pico firmware for service maps")
    dsp = manifest.get("dsp", {})
    domains = dsp.get("service_domains", [])
    if len(domains) != len(MAP_DOMAINS) or {item["file"] for item in domains} != set(MAP_DOMAINS):
        raise ValueError("Unexpected Pico service map inventory")
    records = {}
    for domain in domains:
        name = domain["file"]
        processor, subdomain, instance = MAP_DOMAINS[name]
        if domain.get("domain") != {"soc": "msm", "domain": processor, "subdomain": subdomain, "qmi_instance_id": instance}:
            raise ValueError("Unexpected Pico service map domain: " + name)
        matches = [item for item in dsp.get("files", []) if item.get("path") == name]
        if (len(matches) != 1 or type(matches[0].get("bytes")) is not int or matches[0]["bytes"] <= 0 or
                not re.fullmatch(r"[0-9a-f]{64}", matches[0].get("sha256", "")) or
                [item for item in manifest.get("files", []) if item.get("path") == name] != matches):
            raise ValueError("Invalid Pico service map record: " + name)
        records[name] = matches[0]
    return "".join(records[name]["sha256"] + "  /usr/lib/firmware/" + name + "\n" for name in MAP_DOMAINS).encode()


def regular(root, name):
    path = root / name
    if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Expected a regular Pico runtime file: " + name)
    return path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_files(source, mapper=False):
    source = Path(source)
    link = source / LIBRARY_LINK
    if not link.is_symlink() or str(link.readlink()) != LIBRARY_TARGET:
        raise ValueError("Unexpected QRTR library ABI link")
    provenance = PROVENANCE | (MAPPER_PROVENANCE if mapper else {})
    runtime = RUNTIME + ((MAPPER,) if mapper else ())
    licenses = LICENSES + ((MAPPER_LICENSE,) if mapper else ())
    for name, reference in provenance.items():
        if regular(source, name).read_bytes() != (ROOT / reference).read_bytes():
            raise ValueError("Pico QRTR source identity mismatch: " + name)
    for name in runtime:
        path = regular(source, name)
        header = path.read_bytes()[:20]
        if header[:7] != b"\x7fELF\x02\x01\x01" or header[18:20] != b"\xb7\x00":
            raise ValueError("Expected a Linux ARM64 QRTR ELF: " + name)
        if stat.S_IMODE(path.stat().st_mode) != 0o755:
            raise ValueError("Unexpected QRTR executable mode: " + name)
    files = list(runtime) + list(provenance) + list(licenses)
    for name in licenses:
        if not regular(source, name).read_bytes().strip():
            raise ValueError("Missing QRTR license: " + name)
    return {name: regular(source, name) for name in files}


def install(source, root, firmware=None):
    root = Path(root)
    if (root / "etc/initrd-release").exists() or (root / "etc/initrd-release").is_symlink():
        raise ValueError("Pico root services require a real root, not an initramfs")
    if any(root.glob("usr/lib/systemd/system/armada-quest*")) or any(root.glob("etc/systemd/system/**/armada-quest*")):
        raise ValueError("Pico root cannot contain Quest startup units")
    checksums = service_maps(firmware)
    files = source_files(source, mapper=checksums is not None)
    files.update({
        "usr/libexec/armada-pico-qrtr": ROOT / "system/pico/armada-pico-qrtr",
        "usr/lib/systemd/system/armada-pico-qrtr.service": ROOT / "system/pico/armada-pico-qrtr-root.service",
    })
    if checksums is not None:
        for line in checksums.decode().splitlines():
            checksum, name = line.split("  ")
            if digest(regular(root, name.lstrip("/"))) != checksum:
                raise ValueError("Changed installed Pico service map: " + name)
        files.update({MAP_CHECKSUMS: checksums,
                      "usr/libexec/armada-pico-pd-mapper": ROOT / "system/pico/armada-pico-pd-mapper",
                      "usr/lib/systemd/system/armada-pico-pd-mapper.service":
                          ROOT / "system/pico/armada-pico-pd-mapper-root.service"})
    expected = []
    for name, path in files.items():
        mode = 0o755 if name in (*RUNTIME, MAPPER) or name.startswith("usr/libexec/") else 0o644
        destination = root / name
        if destination.is_symlink() or not destination.resolve().is_relative_to(root.resolve()):
            raise ValueError("Unsafe Pico runtime destination: " + name)
        if destination.exists() and not destination.is_file():
            raise ValueError("Pico runtime destination is not a file: " + name)
        expected.append({"path": name, "sha256": hashlib.sha256(path).hexdigest() if isinstance(path, bytes) else digest(path), "mode": mode})
    links = {LIBRARY_LINK: LIBRARY_TARGET,
             "etc/systemd/system/multi-user.target.wants/armada-pico-qrtr.service":
                 "/usr/lib/systemd/system/armada-pico-qrtr.service"}
    if checksums is not None:
        links["etc/systemd/system/multi-user.target.wants/armada-pico-pd-mapper.service"] = \
            "/usr/lib/systemd/system/armada-pico-pd-mapper.service"
    for name, target in links.items():
        link = root / name
        if not link.parent.resolve().is_relative_to(root.resolve()):
            raise ValueError("Unsafe Pico runtime link parent: " + name)
        if link.exists() or link.is_symlink():
            if name != LIBRARY_LINK or not link.is_symlink() or str(link.readlink()) != target:
                raise ValueError("Pico runtime link already exists: " + name)
    for item in expected:
        destination = root / item["path"]
        destination.parent.mkdir(parents=True, exist_ok=True)
        source_file = files[item["path"]]
        if isinstance(source_file, bytes):
            destination.write_bytes(source_file)
        else:
            shutil.copyfile(source_file, destination)
        destination.chmod(item["mode"])
    for name, target in links.items():
        link = root / name
        link.parent.mkdir(parents=True, exist_ok=True)
        if not link.is_symlink():
            link.symlink_to(target)
    result = {"files": expected, "links": links,
              "required_cmdline": "armada.pico=qrtr", "pd_mapper": checksums is not None, "dsp_startup": False}
    verify(root, result)
    return result


def verify(root, manifest):
    root = Path(root)
    for item in manifest["files"]:
        path = regular(root, item["path"])
        if digest(path) != item["sha256"] or stat.S_IMODE(path.stat().st_mode) != item["mode"]:
            raise ValueError("Changed Pico root service file: " + item["path"])
    for name, target in manifest["links"].items():
        link = root / name
        if not link.parent.resolve().is_relative_to(root.resolve()) or not link.is_symlink() or str(link.readlink()) != target:
            raise ValueError("Changed Pico root service link: " + name)


def verify_image(image, manifest, destination):
    destination = Path(destination)
    if any(c in str(destination) for c in ('"', "\\", "\n", "\r")):
        raise ValueError("Invalid Pico runtime readback directory")
    destination.mkdir(parents=True, exist_ok=False)
    names = [item["path"] for item in manifest["files"]] + list(manifest["links"])
    for name in names:
        if (not name or Path(name).is_absolute() or ".." in Path(name).parts or
                any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-._/" for c in name)):
            raise ValueError("Invalid Pico runtime image path")
        parent = destination / Path(name).parent
        parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["debugfs", "-R", f'rdump /{name} "{parent}"', str(image)],
                       check=True, timeout=15, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    verify(destination, manifest)
