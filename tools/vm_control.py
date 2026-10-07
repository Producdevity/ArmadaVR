#!/usr/bin/env python3
"""Inspect or control this project's desktop VM through its local QMP socket."""
import argparse
import base64
from contextlib import closing
import hashlib
import json
import shlex
import socket
import time
import uuid
from pathlib import Path


def request(directory, command, arguments=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(5)
        connection.connect(str(Path(directory).resolve() / "qmp.sock"))
        with closing(connection.makefile("rwb")) as stream:
            if "QMP" not in json.loads(stream.readline()):
                raise ValueError("Expected a QEMU monitor greeting")
            for name, args in (("qmp_capabilities", {}), (command, arguments or {})):
                stream.write((json.dumps({"execute": name, "arguments": args}) + "\n").encode())
                stream.flush()
                while True:
                    response = json.loads(stream.readline())
                    if "error" in response:
                        raise ValueError(response["error"])
                    if "return" in response:
                        break
            return response["return"]


def sync_agent(connection, stream):
    nonce = uuid.uuid4().int & ((1 << 63) - 1)
    request = {"execute": "guest-sync-delimited", "arguments": {"id": nonce}}
    stream.write(b"\xff" + (json.dumps(request) + "\n").encode())
    stream.flush()
    deadline = time.monotonic() + 10
    pending = None
    while time.monotonic() < deadline:
        connection.settimeout(max(0.001, deadline - time.monotonic()))
        chunk = stream.readline(65536)
        if not chunk:
            raise ConnectionError("Guest agent disconnected during protocol synchronization")
        if b"\xff" in chunk:
            pending = chunk.rsplit(b"\xff", 1)[-1]
        elif pending is not None:
            pending += chunk
        if pending is None:
            continue
        if len(pending) > 4096:
            pending = None
            continue
        if not chunk.endswith(b"\n"):
            continue
        try:
            response = json.loads(pending)
        except (ValueError, UnicodeDecodeError):
            response = None
        pending = None
        if isinstance(response, dict) and response.get("return") == nonce:
            connection.settimeout(10)
            return
    raise TimeoutError("Guest agent did not synchronize within 10 seconds")


def agent_request(directory, command, arguments=None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(10)
        connection.connect(str(Path(directory).resolve() / "agent.sock"))
        with closing(connection.makefile("rwb")) as stream:
            sync_agent(connection, stream)
            stream.write((json.dumps({"execute": command, "arguments": arguments or {}}) + "\n").encode())
            stream.flush()
            line = stream.readline()
            if not line:
                raise ConnectionError("Guest agent disconnected before replying; the VM may be shutting down")
            result = json.loads(line)
            if "error" in result:
                raise ValueError(result["error"])
            return result["return"]


def guest_exec(directory, command):
    process = agent_request(directory, "guest-exec", {
        "path": "/usr/bin/timeout",
        "arg": ["--kill-after=2s", "8s", "/bin/bash", "-lc", command],
        "capture-output": True,
    })
    for _ in range(150):
        status = agent_request(directory, "guest-exec-status", {"pid": process["pid"]})
        if status["exited"]:
            for key in ("out-data", "err-data"):
                if key in status:
                    print(base64.b64decode(status[key]).decode(errors="replace"), end="")
            return status.get("exitcode", 1)
        time.sleep(0.1)
    raise TimeoutError("No guest command result within 15 seconds; execution is limited to 8 seconds")


def put_file(directory, source, destination):
    source = Path(source)
    if source.is_symlink() or not source.is_file() or not destination.startswith("/"):
        raise ValueError("Expected a regular local file and an absolute guest destination")
    temporary = destination + ".upload-" + uuid.uuid4().hex
    handle = agent_request(directory, "guest-file-open", {"path": temporary, "mode": "wb"})
    digest = hashlib.sha256()
    try:
        with source.open("rb") as stream:
            while chunk := stream.read(1024**2):
                digest.update(chunk)
                result = agent_request(directory, "guest-file-write", {
                    "handle": handle, "buf-b64": base64.b64encode(chunk).decode(),
                })
                if result["count"] != len(chunk):
                    raise OSError("Incomplete write to the guest file")
    finally:
        agent_request(directory, "guest-file-close", {"handle": handle})
    temp = shlex.quote(temporary)
    target = shlex.quote(destination)
    checksum = shlex.quote(digest.hexdigest() + "  " + temporary)
    command = f"printf '%s\\n' {checksum} | sha256sum -c - && ln -T -- {temp} {target} && rm -- {temp}"
    if guest_exec(directory, command):
        raise OSError(f"Guest copy verification/install failed; temporary file: {temporary}")
    print(f"Copied {source} to VM:{destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("action", choices=("status", "screenshot", "poweroff", "exec", "put"))
    parser.add_argument("command", nargs="?", help="Shell command inside this VM, executed as root")
    parser.add_argument("destination", nargs="?", help="Absolute guest destination for put; existing files are refused")
    args = parser.parse_args()
    try:
        if args.action == "put":
            if args.command is None or args.destination is None:
                parser.error("put requires a local file and a guest destination")
            put_file(args.directory, args.command, args.destination)
            return 0
        if args.action == "exec":
            if args.command is None:
                parser.error("exec requires a guest command")
            return guest_exec(args.directory, args.command)
        if args.action == "screenshot":
            path = args.directory.resolve() / "desktop.png"
            request(args.directory, "screendump", {"filename": str(path), "format": "png"})
            print(path)
        elif args.action == "poweroff":
            # The vendor-derived QEMU kernel has no ACPI power-button handler.
            agent_request(args.directory, "guest-exec", {
                "path": "/usr/bin/systemctl", "arg": ["--no-block", "poweroff"],
            })
            print("Guest shutdown requested; use status to confirm the VM has stopped.")
        else:
            print(request(args.directory, "query-status"))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
