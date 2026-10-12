"""Install the pinned Pico QRTR runtime into an offline headset root."""
import hashlib
from pathlib import Path
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


def regular(root, name):
    path = root / name
    if path.is_symlink() or not path.is_file() or not path.resolve().is_relative_to(root.resolve()):
        raise ValueError("Expected a regular Pico runtime file: " + name)
    return path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_files(source):
    source = Path(source)
    link = source / LIBRARY_LINK
    if not link.is_symlink() or str(link.readlink()) != LIBRARY_TARGET:
        raise ValueError("Unexpected QRTR library ABI link")
    for name, reference in PROVENANCE.items():
        if regular(source, name).read_bytes() != (ROOT / reference).read_bytes():
            raise ValueError("Pico QRTR source identity mismatch: " + name)
    for name in RUNTIME:
        path = regular(source, name)
        header = path.read_bytes()[:20]
        if header[:7] != b"\x7fELF\x02\x01\x01" or header[18:20] != b"\xb7\x00":
            raise ValueError("Expected a Linux ARM64 QRTR ELF: " + name)
        if stat.S_IMODE(path.stat().st_mode) != 0o755:
            raise ValueError("Unexpected QRTR executable mode: " + name)
    files = list(RUNTIME) + list(PROVENANCE) + list(LICENSES)
    for name in LICENSES:
        if not regular(source, name).read_bytes().strip():
            raise ValueError("Missing QRTR license: " + name)
    return {name: regular(source, name) for name in files}


def install(source, root):
    root = Path(root)
    if (root / "etc/initrd-release").exists() or (root / "etc/initrd-release").is_symlink():
        raise ValueError("Pico root services require a real root, not an initramfs")
    if any(root.glob("usr/lib/systemd/system/armada-quest*")) or any(root.glob("etc/systemd/system/**/armada-quest*")):
        raise ValueError("Pico root cannot contain Quest startup units")
    files = source_files(source)
    files.update({
        "usr/libexec/armada-pico-qrtr": ROOT / "system/pico/armada-pico-qrtr",
        "usr/lib/systemd/system/armada-pico-qrtr.service": ROOT / "system/pico/armada-pico-qrtr-root.service",
    })
    expected = []
    for name, path in files.items():
        mode = 0o755 if name in RUNTIME or name == "usr/libexec/armada-pico-qrtr" else 0o644
        destination = root / name
        if destination.is_symlink() or not destination.resolve().is_relative_to(root.resolve()):
            raise ValueError("Unsafe Pico runtime destination: " + name)
        if destination.exists() and not destination.is_file():
            raise ValueError("Pico runtime destination is not a file: " + name)
        expected.append({"path": name, "sha256": digest(path), "mode": mode})
    links = {LIBRARY_LINK: LIBRARY_TARGET,
             "etc/systemd/system/multi-user.target.wants/armada-pico-qrtr.service":
                 "/usr/lib/systemd/system/armada-pico-qrtr.service"}
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
        shutil.copyfile(files[item["path"]], destination)
        destination.chmod(item["mode"])
    for name, target in links.items():
        link = root / name
        link.parent.mkdir(parents=True, exist_ok=True)
        if not link.is_symlink():
            link.symlink_to(target)
    result = {"files": expected, "links": links,
              "required_cmdline": "armada.pico=qrtr", "dsp_startup": False}
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
