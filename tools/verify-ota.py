#!/usr/bin/env python3
"""Authenticate a local Android OTA against independently obtained device certificates."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import subprocess
import tempfile
import zipfile


def open_regular(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    stream = os.fdopen(fd, "rb")
    info = os.fstat(fd)
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= limit:
        stream.close()
        raise ValueError(f"Expected a nonempty regular file of at most {limit} bytes: {path}")
    return stream


def read_exact(stream, size):
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated input")
    return data


def signature_region(stream):
    size = os.fstat(stream.fileno()).st_size
    if size < 28:
        raise ValueError("OTA is too short")
    stream.seek(-6, 2)
    signature_start, marker, comment_size = struct.unpack("<HHH", read_exact(stream, 6))
    if marker != 65535 or not 6 < signature_start <= comment_size or comment_size + 22 > size:
        raise ValueError("Invalid Android whole-file signature footer")
    stream.seek(-comment_size - 22, 2)
    eocd = read_exact(stream, comment_size + 22)
    if eocd[:4] != b"PK\x05\x06" or b"PK\x05\x06" in eocd[4:]:
        raise ValueError("Invalid or ambiguous ZIP end record")
    if struct.unpack_from("<H", eocd, 20)[0] != comment_size:
        raise ValueError("ZIP comment length disagrees with signature footer")
    disk, directory_disk, disk_entries, entries, directory_size, directory_offset = struct.unpack_from("<HHHHII", eocd, 4)
    if (disk or directory_disk or disk_entries != entries or entries == 65535 or
            directory_offset + directory_size != size - len(eocd)):
        raise ValueError("Unsupported ZIP directory geometry")
    return size - comment_size - 2, eocd[-signature_start:-6]


def certificates(stream, openssl):
    result, identities = [], []
    with zipfile.ZipFile(stream) as archive:
        entries = archive.infolist()
        names = [entry.filename for entry in entries]
        if not 0 < len(entries) <= 32 or len(set(names)) != len(names):
            raise ValueError("Invalid certificate archive entries")
        for entry in entries:
            if entry.is_dir():
                continue
            if not entry.filename.endswith("x509.pem") or not 0 < entry.file_size <= 128 * 1024:
                raise ValueError("Expected bounded x509.pem certificate entries")
            data = archive.read(entry)
            if not re.fullmatch(rb"\s*-----BEGIN CERTIFICATE-----[A-Za-z0-9+/=\s]+-----END CERTIFICATE-----\s*", data):
                raise ValueError("Expected exactly one PEM certificate per trust entry")
            command = subprocess.run([openssl, "x509", "-outform", "DER"], input=data, capture_output=True, check=True)
            identities.append({"entry": entry.filename, "certificate_der_sha256": hashlib.sha256(command.stdout).hexdigest()})
            result.append(data.strip() + b"\n")
    if not result:
        raise ValueError("No device trust certificates")
    return b"".join(result), identities


def der_fields(data):
    fields, cursor = [], 0
    while cursor < len(data):
        if len(fields) >= 128 or cursor + 2 > len(data):
            raise ValueError("Invalid DER bounds")
        tag, length = data[cursor:cursor + 2]
        cursor += 2
        if tag & 31 == 31:
            raise ValueError("Unsupported DER tag")
        if length & 128:
            count = length & 127
            if not 1 <= count <= 4 or cursor + count > len(data) or data[cursor] == 0:
                raise ValueError("Invalid DER length")
            length = int.from_bytes(data[cursor:cursor + count], "big")
            cursor += count
            if length < 128:
                raise ValueError("Noncanonical DER length")
        if cursor + length > len(data):
            raise ValueError("Truncated DER value")
        fields.append((tag, data[cursor:cursor + length]))
        cursor += length
    return fields


def check_signature_format(signature):
    def children(field, tag):
        if field[0] != tag:
            raise ValueError("Unsupported PKCS7 signature structure")
        return der_fields(field[1])

    def algorithm(field, oid):
        items = children(field, 48)
        if items not in ([(6, oid)], [(6, oid), (5, b"")]):
            raise ValueError("Unsupported PKCS7 signature algorithm")

    outer = der_fields(signature)
    if len(outer) != 1:
        raise ValueError("Trailing PKCS7 data")
    content = children(outer[0], 48)
    if len(content) != 2 or content[0] != (6, bytes.fromhex("2a864886f70d010702")):
        raise ValueError("Expected PKCS7 SignedData")
    explicit = children(content[1], 160)
    if len(explicit) != 1:
        raise ValueError("Invalid PKCS7 content")
    signed = children(explicit[0], 48)
    if not 4 <= len(signed) <= 6 or signed[0] != (2, b"\x01"):
        raise ValueError("Unsupported PKCS7 SignedData version")
    sha256 = bytes.fromhex("608648016503040201")
    digests = children(signed[1], 49)
    if len(digests) != 1:
        raise ValueError("Expected one signature digest")
    algorithm(digests[0], sha256)
    if children(signed[2], 48) != [(6, bytes.fromhex("2a864886f70d010701"))]:
        raise ValueError("Expected detached PKCS7 content")
    if [field[0] for field in signed[3:-1]] not in ([], [160], [161], [160, 161]):
        raise ValueError("Unsupported PKCS7 certificate fields")
    signers = children(signed[-1], 49)
    if len(signers) != 1:
        raise ValueError("Expected one OTA signer")
    signer = children(signers[0], 48)
    # Android recovery verifies the raw whole-file digest, not CMS signed attributes.
    if len(signer) != 5 or signer[0] != (2, b"\x01") or signer[1][0] != 48 or signer[-1][0] != 4:
        raise ValueError("Expected an OTA signer without signed or unsigned attributes")
    algorithm(signer[2], sha256)
    algorithm(signer[3], bytes.fromhex("2a864886f70d010101"))
    if not 256 <= len(signer[-1][1]) <= 1024:
        raise ValueError("Unsupported RSA signature size")


def authenticate(stream, signature, signed_length, trust, openssl):
    check_signature_format(signature)
    with tempfile.TemporaryDirectory(prefix="armadavr-ota-") as workspace:
        directory = Path(workspace)
        (directory / "signature.der").write_bytes(signature)
        (directory / "trusted.pem").write_bytes(trust)
        # Ignore package certificates and verify only with independently supplied keys.
        args = [openssl, "cms", "-verify", "-binary", "-inform", "DER", "-in", str(directory / "signature.der"),
                "-content", "/dev/stdin", "-certfile", str(directory / "trusted.pem"), "-nointern", "-noverify", "-out", os.devnull]
        with (directory / "verify.log").open("w+b") as log:
            process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=log)
            try:
                stream.seek(0)
                remaining = signed_length
                try:
                    while remaining:
                        data = read_exact(stream, min(remaining, 1024**2))
                        process.stdin.write(data)
                        remaining -= len(data)
                except BrokenPipeError:
                    pass
                finally:
                    try:
                        process.stdin.close()
                    except BrokenPipeError:
                        pass
                code = process.wait(timeout=60)
                if code or remaining:
                    log.seek(0)
                    raise ValueError("OTA signature does not verify with supplied device certificates: " + log.read(4096).decode(errors="replace").strip())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


def key_values(data):
    result = {}
    for line in data.decode("utf-8").splitlines():
        key, value = line.split("=", 1)
        if not key or key in result:
            raise ValueError("Empty or duplicate metadata field")
        result[key] = value
    return result


def verify(path, trust_path, expected_device, expected_build, openssl="openssl"):
    if not re.fullmatch(r"[A-Za-z0-9_-]+", expected_device) or not re.fullmatch(r"[0-9]+", expected_build):
        raise ValueError("Expected an explicit device codename and numeric incremental build")
    with open_regular(path, 16 * 1024**3) as stream, open_regular(trust_path, 4 * 1024**2) as anchors:
        before = os.fstat(stream.fileno())
        trust, identities = certificates(anchors, openssl)
        signed_length, signature = signature_region(stream)
        authenticate(stream, signature, signed_length, trust, openssl)
        with zipfile.ZipFile(stream) as archive:
            entries = archive.infolist()
            if len(entries) > 128 or len({entry.filename for entry in entries}) != len(entries):
                raise ValueError("Excessive or duplicate OTA entries")
            def small(name):
                item = archive.getinfo(name)
                if not 0 < item.file_size <= 1024**2:
                    raise ValueError("OTA metadata size limit")
                return archive.read(item)
            metadata = key_values(small("META-INF/com/android/metadata"))
            if (metadata.get("ota-type") != "AB" or metadata.get("pre-device") != expected_device or
                    metadata.get("post-build-incremental") != expected_build):
                raise ValueError("Authenticated OTA device, type or build mismatch")
            properties = key_values(small("payload_properties.txt"))
            payload = archive.getinfo("payload.bin")
            if (payload.compress_type != zipfile.ZIP_STORED or payload.flag_bits & 1 or
                    payload.file_size != int(properties["FILE_SIZE"]) or not 24 <= payload.file_size <= 16 * 1024**3):
                raise ValueError("Unsupported payload encoding or size mismatch")
            metadata_size = int(properties["METADATA_SIZE"])
            if not 24 <= metadata_size <= min(payload.file_size, 8 * 1024**2):
                raise ValueError("Payload metadata size limit")
            with archive.open(payload) as data:
                header = read_exact(data, 24)
                magic, version, manifest_length, signature_length = struct.unpack(">4sQQI", header)
                if (magic != b"CrAU" or version != 2 or 24 + manifest_length != metadata_size or
                        signature_length > 1024**2 or metadata_size + signature_length > payload.file_size):
                    raise ValueError("Payload header disagrees with authenticated metadata")
                manifest = header + read_exact(data, manifest_length)
                metadata_digest = base64.b64encode(hashlib.sha256(manifest).digest()).decode()
                if metadata_digest != properties["METADATA_HASH"]:
                    raise ValueError("Payload metadata hash mismatch")
                digest = hashlib.sha256(manifest)
                while chunk := data.read(4 * 1024**2):
                    digest.update(chunk)
            if base64.b64encode(digest.digest()).decode() != properties["FILE_HASH"]:
                raise ValueError("Payload hash mismatch")
        stream.seek(0)
        archive_hash = hashlib.file_digest(stream, "sha256").hexdigest()
        after = os.fstat(stream.fileno())
        if (before.st_ino, before.st_size, before.st_mtime_ns, before.st_ctime_ns) != (after.st_ino, after.st_size, after.st_mtime_ns, after.st_ctime_ns):
            raise ValueError("OTA changed while verifying")
    return {"scope": "offline Android OTA authentication", "ota_signature_verified_with_supplied_device_certificates": True,
            "trust_certificate_source": "caller-supplied; must be obtained independently of this OTA",
            "certificates": identities, "signature_format": "detached_PKCS7_SHA256_RSA_without_attributes",
            "archive_bytes": before.st_size, "archive_sha256": archive_hash,
            "authenticated_metadata": metadata, "payload_sha256": digest.hexdigest(), "payload_metadata_hash_verified": True,
            "payload_hash_verified": True, "signed_bytes": signed_length, "device_rollback_acceptance_verified": False,
            "headset_boot_acceptance_verified": False, "recovery_restore_verified": False, "flash_ready": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ota", type=Path)
    parser.add_argument("--device-certificates", type=Path, required=True, help="Independently obtained stock otacerts.zip")
    parser.add_argument("--expected-device", required=True)
    parser.add_argument("--expected-build", required=True)
    parser.add_argument("--openssl", default="openssl")
    args = parser.parse_args()
    try:
        report = verify(args.ota, args.device_certificates, args.expected_device, args.expected_build, args.openssl)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
