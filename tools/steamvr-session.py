#!/usr/bin/env python3
"""Run a bounded SteamVR session using the QEMU software Vulkan backend."""
import argparse
import fcntl
import hashlib
import json
import os
import platform
from pathlib import Path
import signal
import secrets
import shutil
import subprocess
import time
import tempfile


# This verified build predates FEX #5856 and executes stale code after cache resizing.
FEX_BROWSER_CACHE_SHA256 = "f9cbb7a70e804b9930fed02fd9d1349d49d7808faace703063a9b4944bc3568e"


def prepare_fex_server(config, rootfs):
    runtime = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
    if runtime.is_symlink() or not runtime.is_dir() or runtime.stat().st_uid != os.getuid():
        raise ValueError("FEX isolation requires an owned runtime directory")
    identity = hashlib.sha256(os.fsencode(str(config.resolve()) + "\0" + str(rootfs.resolve()))).hexdigest()[:16]
    parent = runtime / "armada-vr-fex"
    data = parent / identity
    socket = data / "Server" / f"{os.getuid()}.FEXServer.Socket"
    if len(os.fsencode(socket)) > 107:
        raise ValueError("FEX server socket path exceeds the Unix socket limit")
    for directory in (parent, data):
        if directory.is_symlink():
            raise ValueError("FEX server directories must be regular owned paths")
        directory.mkdir(mode=0o700, exist_ok=True)
        if directory.stat().st_uid != os.getuid() or directory.stat().st_mode & 0o077:
            raise ValueError("FEX server directories must be private and owned")
    return {"FEX_APP_DATA_LOCATION": str(data) + "/",
            "FEX_SERVERSOCKETPATH": f"armada-vr-{os.getuid()}-{identity}"}


def prepare_browser_config(config, interpreter_sha256):
    if interpreter_sha256 != FEX_BROWSER_CACHE_SHA256:
        return False
    key = "DynamicL1CacheDecreaseCountHeuristic"
    if os.environ.get("FEX_DYNAMICL1CACHEDECREASECOUNTHEURISTIC") not in (None, "0"):
        raise ValueError("Inherited FEX cache-shrink setting overrides the verified browser guard")
    directory = config / "AppConfig"
    path = directory / "vrwebhelper.json"
    if config.is_symlink() or directory.is_symlink() or path.is_symlink():
        raise ValueError("Browser AppConfig must use regular paths inside the private session")
    if path.exists():
        if not path.is_file() or path.stat().st_size > 65536:
            raise ValueError("Invalid existing browser AppConfig")
        settings = json.loads(path.read_text())
        values = settings.get("Config") if isinstance(settings, dict) else None
        if not isinstance(values, dict) or values.get(key) != "0":
            raise ValueError("Existing browser AppConfig conflicts with the verified cache guard")
    else:
        directory.mkdir(mode=0o700, exist_ok=True)
        with path.open("x") as stream:
            stream.write(json.dumps({"Config": {key: "0"}}, indent=2) + "\n")
    return True


def prepare_steam_client(directory, client, fex, rootfs, icd, render_node, registry, runtime,
                         bwrap=Path("/usr/bin/bwrap")):
    if hashlib.sha256(fex.read_bytes()).hexdigest() != FEX_BROWSER_CACHE_SHA256:
        raise ValueError("Steam client graphics require the verified FEX-2607-76 interpreter")
    if client.resolve() != (Path.home() / ".local/share/Steam").resolve():
        raise ValueError("Steam client must belong to the current HOME profile")
    if not (client / "steam.sh").is_file() or not (client / "steamapps/appmanifest_250820.acf").is_file():
        raise ValueError("Steam client needs its launcher and complete SteamVR installation record")
    installed = client / "steamapps/common/SteamVR"
    if not (installed / "bin/linux64/vrclient.so").samefile(runtime / "bin/linux64/vrclient.so"):
        raise ValueError("Steam client and backend must use the same installed SteamVR runtime")
    fex_root = fex.parent.parent
    emulator = fex_root.parent / "emulator.json"
    host = fex_root / "lib/aarch64-linux-gnu/fex-emu/HostThunks"
    guest = fex_root / "share/fex-emu/GuestThunks"
    for path in (emulator, host / "libGL-host.so", guest / "libGL-guest.so",
                 host / "libvulkan-host.so", guest / "libvulkan-guest.so"):
        if not path.is_file():
            raise ValueError(f"Missing client translation dependency: {path}")
    header = bwrap.read_bytes()[:20]
    if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
        raise ValueError("The client requires native AArch64 bubblewrap")
    directory.mkdir(mode=0o700)
    config = directory / "fex"
    config.mkdir(mode=0o700)
    (config / "AppConfig").mkdir(mode=0o700)
    helper = directory / "native-bwrap"
    shutil.copyfile(bwrap, helper)
    helper.chmod(0o755)
    (config / "Config.json").write_text(json.dumps({"Config": {
        "RootFS": str(rootfs), "ThunkHostLibs": str(host), "ThunkGuestLibs": str(guest),
        "SilentLog": "1", "OutputLog": "stderr"}, "ThunksDB": {"GL": "0", "Vulkan": "1"}}) + "\n")
    (config / "AppConfig/steam.json").write_text(json.dumps({"ThunksDB": {"GL": "0"}}) + "\n")
    (config / "AppConfig/steamwebhelper.json").write_text(json.dumps({"Config": {
        "DynamicL1CacheDecreaseCountHeuristic": "0"}, "ThunksDB": {"GL": "1"}}) + "\n")
    aliases = ("/usr/lib", "/usr/lib/x86_64-linux-gnu", "/var/pressure-vessel/gfx/main/usr/lib",
               "/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu")
    libraries = {"Vulkan": ("libvulkan-guest.so", ("libvulkan.so", "libvulkan.so.1", "libvulkan.so.1.4.357")),
                 "GL": ("libGL-guest.so", ("libGL.so", "libGL.so.1", "libGL.so.1.2.0", "libGL.so.1.7.0"))}
    (config / "ThunksDB.json").write_text(json.dumps({"DB": {
        name: {"Library": library, "Overlay": [prefix + "/" + alias for prefix in aliases for alias in names]}
        for name, (library, names) in libraries.items()}}) + "\n")
    host_env = ("VK_DRIVER_FILES=" + str(icd), "VK_ICD_FILENAMES=" + str(icd),
                "LVP_DRM_SYNC=" + str(render_node), "LP_NUM_THREADS=2", "MESA_VK_WSI_SW_PRESENT=1",
                "LIBGL_DRIVERS_PATH=/usr/lib64/dri")
    settings = ['"TSOEnabled":"1"', '"Multiblock":"1"', '"SilentLog":"1"']
    # This FEX version reads repeated string keys; arrays silently lose HostEnv entries.
    settings.extend('"HostEnv":' + json.dumps(value) for value in host_env)
    application = config / "application.json"
    application.write_text('{"Config":{' + ','.join(settings) + '},"ThunksDB":{"Vulkan":"1"},'
        '"AppOverrides":{"steamwebhelper":{"HostEnv":"MESA_LOADER_DRIVER_OVERRIDE=zink",'
        '"HostEnv":"LIBGL_KOPPER_DRI2=true"}}}\n')
    env = os.environ | {"FEX_APP_CONFIG_LOCATION": str(config) + "/", "FEX_PORTABLE": "1",
        "FEX_APP_CONFIG": str(application), "VR_OVERRIDE": str(runtime), "VR_PATHREG_OVERRIDE": str(registry),
        "STEAM_FORCE_CLIENT": "steamrt64", "PRESSURE_VESSEL_BWRAP": str(helper),
        "STEAM_COMPAT_EMULATOR": str(emulator), "LIBGL_ALWAYS_SOFTWARE": "1"}
    for name in ("LD_LIBRARY_PATH", "LD_PRELOAD", "STEAM_RUNTIME", "FEX_ENV", "FEX_GDBSERVER",
                 "FEX_ROOTFS", "FEX_THUNKHOSTLIBS", "FEX_THUNKGUESTLIBS",
                 "FEX_DYNAMICL1CACHEDECREASECOUNTHEURISTIC", "FEX_APP_DATA_LOCATION", "FEX_SERVERSOCKETPATH"):
        env.pop(name, None)
    command = [str(fex), "/bin/bash", str(client / "steam.sh"), "-nobootstrapupdate",
               "-skipinitialbootstrap", "-publicbeta", "-no-cef-sandbox", "-vrverbose"]
    return command, env


def stop(process):
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        process.poll()
        return
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        process.poll()
        try:
            os.killpg(process.pid, 0)
        except ProcessLookupError:
            process.wait(timeout=5)
            return
        except PermissionError:
            # macOS can return EPERM while a signaled process is exiting.
            pass
        time.sleep(0.05)
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    process.wait(timeout=5)


def wait_for_startup(processes, logfile, offset, timeout, marker=b"Startup Complete ("):
    deadline = time.monotonic() + timeout
    pending = b""
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"{process.args[1]} exited with {process.returncode} before compositor startup")
        if logfile.exists():
            with logfile.open("rb") as stream:
                if os.fstat(stream.fileno()).st_size < offset:
                    offset, pending = 0, b""
                stream.seek(offset)
                pending += stream.read(65536)
                offset = stream.tell()
            if marker in pending:
                return
            pending = pending[-256:]
        time.sleep(0.25)
    raise TimeoutError("SteamVR component did not finish startup before the deadline")


def prepare_virtual_session(bundle, binaries, state):
    profile = json.loads((bundle / "steamvr-presentation.json").read_text())
    with (binaries / "vrcompositor").open("rb") as stream:
        checksum = hashlib.file_digest(stream, "sha256").hexdigest()
    if checksum != profile["compositor_sha256"]:
        raise ValueError("Virtual XPresent bootstrap requires the verified SteamVR 2.16.7 compositor")
    settings = json.loads((bundle / "steamvr-virtual.json").read_text())
    if settings.get("steamvr", {}).get("forcedDriver") != "null" or not settings.get("driver_null", {}).get("enable"):
        raise ValueError("Virtual presentation requires Valve's null headset profile")
    for path in (bundle / "xpresent/libXpresent.so.1", bundle / "libvulkan-procaddr.so", bundle / "steamvr-probe",
                 bundle / "armada_virtual/driver.vrdrivermanifest"):
        if not path.is_file():
            raise ValueError(f"Missing virtual SteamVR dependency: {path}")
    directory = state / "steamvr-virtual"
    for name in ("config", "logs"):
        (directory / name).mkdir(parents=True, exist_ok=True)
    settings_path = directory / "config/steamvr.vrsettings"
    if settings_path.exists():
        previous = json.loads(settings_path.read_text())
        if "GpuSpeed" in previous:
            settings["GpuSpeed"] = previous["GpuSpeed"]
    settings_path.write_text(json.dumps(settings, indent=2) + "\n")
    registry = directory / "openvrpaths.vrpath"
    registry.write_text(json.dumps({"version": 1, "jsonid": "vrpathreg",
        "runtime": [str(binaries.parent.parent)], "config": [str(directory / "config")],
        "log": [str(directory / "logs")], "external_drivers": [str(bundle / "armada_virtual")]}) + "\n")
    return registry, directory / "logs/vrcompositor.txt"


def prepare_runtime_libraries(bundle, client, directory):
    libraries = json.loads((bundle / "steamvr-runtime.json").read_text())["libraries"]
    verified = []
    for library in libraries:
        name, relative = library["name"], Path(library["source"])
        if Path(name).name != name or name in (".", "..") or relative.is_absolute() or ".." in relative.parts:
            raise ValueError("Invalid SteamVR runtime library path")
        source = client / relative
        with source.open("rb") as stream:
            header = stream.read(20)
            stream.seek(0)
            checksum = hashlib.file_digest(stream, "sha256").hexdigest()
        if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\x3e\x00":
            raise ValueError(f"SteamVR runtime library is not x86-64 ELF: {source}")
        if checksum != library["sha256"]:
            raise ValueError(f"SteamVR runtime library checksum mismatch: {source}")
        verified.append((name, source.resolve()))
    target = directory / "libraries"
    target.mkdir()
    for name, source in verified:
        (target / name).symlink_to(source)
    return target


def start_display(mode, directory, env, log):
    number = next((number for number in range(88, 100)
                   if not Path(f"/tmp/.X11-unix/X{number}").exists() and
                   not Path(f"/tmp/.X{number}-lock").exists()), None)
    if number is None:
        raise RuntimeError("No free virtual X display")
    display = f":{number}"
    authority = directory / "Xauthority"
    authority.touch(mode=0o600, exist_ok=False)
    subprocess.run(["xauth", "-f", str(authority), "add", display, ".", secrets.token_hex(16)],
                   check=True, timeout=10, stdout=subprocess.DEVNULL)
    screen = "1280x800" if mode == "window" else "0"
    command = ["Xephyr" if mode == "window" else "Xvfb", display, "-screen", screen]
    if mode == "headless":
        command.append("1280x800x24")
    command += ["-auth", str(authority), "-nolisten", "tcp", "-noreset"]
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    inner = env | {"DISPLAY": display, "XAUTHORITY": str(authority)}
    try:
        for _ in range(50):
            if process.poll() is not None:
                raise RuntimeError("Virtual display exited during startup")
            if subprocess.run(["xdpyinfo"], env=inner, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=2).returncode == 0:
                return process, inner
            time.sleep(0.1)
        raise TimeoutError("Virtual X display did not become ready")
    except BaseException:
        stop(process)
        raise


def validate_runtime_paths(runtime):
    root = runtime.resolve(strict=True)
    for name in ("vrserver", "vrcompositor", "vrmonitor", "vrclient.so"):
        path = root / "bin/linux64" / name
        if not path.is_file() or not path.resolve(strict=True).is_relative_to(root):
            raise ValueError(f"SteamVR component resolves outside the selected runtime or is missing: {name}")
    manifest = (root / "steamxr_linux64.json").resolve(strict=True)
    # The OpenXR loader resolves the library relative to the canonical manifest.
    if manifest.parent != root:
        raise ValueError("OpenXR manifest must resolve in the selected SteamVR runtime directory")
    library = json.loads(manifest.read_text()).get("runtime", {}).get("library_path")
    if not isinstance(library, str) or (manifest.parent / library).resolve() != (root / "bin/linux64/vrclient.so").resolve():
        raise ValueError("OpenXR manifest does not select this SteamVR runtime's client")


def validate_native_inputs(runtime, bundle):
    runtime = runtime.resolve(strict=True)
    manifest = json.loads((bundle / "build.json").read_text())
    if manifest.get("target") != "steamvr-virtual-aarch64":
        raise ValueError("Native session requires an AArch64 virtual-device bundle")
    required = {"steamvr-probe", "vulkan-interop", "vulkan-external-sync", "steamvr-session.py",
                "steamvr-virtual.json", "armada_virtual/driver.vrdrivermanifest",
                "armada_virtual/resources/rendermodels/controller/controller.json",
                "armada_virtual/bin/linuxarm64/driver_armada_virtual.so"}
    hashes = manifest.get("sha256", {})
    if not required.issubset(hashes):
        raise ValueError("Native bundle is missing its checked session dependencies")
    for name, checksum in hashes.items():
        relative = Path(name)
        path = bundle / relative
        if relative.is_absolute() or ".." in relative.parts or path.is_symlink() or not path.is_file():
            raise ValueError(f"Invalid native bundle path: {name}")
        if not path.resolve().is_relative_to(bundle.resolve()):
            raise ValueError(f"Native bundle resolves outside its directory: {name}")
        with path.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != checksum:
                raise ValueError(f"Native bundle checksum mismatch: {name}")
    for name in ("vrserver", "vrcompositor", "vrclient.so", "libopenvr_api.so"):
        path = runtime / "bin/linuxarm64" / name
        if not path.is_file() or not path.resolve().is_relative_to(runtime):
            raise ValueError(f"Native runtime component resolves outside its directory: {name}")
        with path.open("rb") as stream:
            header = stream.read(20)
        if header[:6] != b"\x7fELF\x02\x01" or header[18:20] != b"\xb7\x00":
            raise ValueError(f"Native runtime component is not AArch64 ELF: {name}")
    path = runtime / "steamxr_linuxarm64.json"
    if not path.is_file() or path.resolve().parent != runtime:
        raise ValueError("Native OpenXR manifest must resolve in the selected runtime")
    library = json.loads(path.read_text()).get("runtime", {}).get("library_path")
    if not isinstance(library, str) or (path.parent / library).resolve() != (runtime / "bin/linuxarm64/vrclient.so").resolve():
        raise ValueError("Native OpenXR manifest does not select this runtime's client")
    settings = json.loads((bundle / "steamvr-virtual.json").read_text())
    steamvr = settings.get("steamvr", {})
    driver = settings.get("driver_armada_virtual", {})
    if steamvr.get("forcedDriver") != "armada_virtual" or driver.get("simulateHeadset") is not True or driver.get("enable") is not True:
        raise ValueError("Native test requires the explicitly enabled simulated headset")
    if any(steamvr.get(name) is not False for name in ("activateMultipleDrivers", "startMonitorFromAppLaunch",
            "startCompositorFromAppLaunch", "startDashboardFromAppLaunch", "startOverlayAppsFromDashboard")):
        raise ValueError("Native test must disable additional drivers and automatic component launch")
    return settings


def run_native(args):
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
        raise ValueError("Native session requires AArch64 Linux")
    if os.geteuid() == 0:
        raise ValueError("Run native checks as an unprivileged user")
    if not args.runtime or args.probe or args.resolver or args.debug_compositor or args.virtual_display or args.steam_client:
        raise ValueError("Native checks require --runtime and do not accept FEX or presentation options")
    runtime, bundle = args.runtime.resolve(strict=True), args.bundle.resolve(strict=True)
    settings = validate_native_inputs(runtime, bundle)
    if not args.icd.is_file() or not args.render_node.exists():
        raise ValueError("Native checks require an existing Vulkan ICD and DRM render node")
    state = Path.home() / ".local/state/armada-vr"
    state.mkdir(parents=True, exist_ok=True)
    with (state / "steamvr-session.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("An Armada VR session is already running") from error
        with tempfile.TemporaryDirectory(prefix="native-", dir=state) as temporary:
            directory = Path(temporary)
            for name in ("config", "logs", "run"):
                (directory / name).mkdir(mode=0o700)
            (directory / "config/steamvr.vrsettings").write_text(json.dumps(settings, indent=2) + "\n")
            registry = directory / "openvrpaths.vrpath"
            registry.write_text(json.dumps({"version": 1, "jsonid": "vrpathreg", "runtime": [str(runtime)],
                "config": [str(directory / "config")], "log": [str(directory / "logs")],
                "external_drivers": [str(bundle / "armada_virtual")]}) + "\n")
            binaries = runtime / "bin/linuxarm64"
            libraries = f"{binaries}:{binaries / 'qt/lib'}"
            env = os.environ | {"VR_PATHREG_OVERRIDE": str(registry), "VR_OVERRIDE": str(runtime),
                "VR_CONFIG_PATH": str(directory / "config"), "VR_LOG_PATH": str(directory / "logs"),
                "XDG_RUNTIME_DIR": str(directory / "run"), "STEAMVR_TOOLSDIR": str(runtime),
                "LD_LIBRARY_PATH": libraries, "VRCOMPOSITOR_LD_LIBRARY_PATH": libraries,
                "VK_DRIVER_FILES": str(args.icd.resolve()), "LVP_DRM_SYNC": str(args.render_node), "LP_NUM_THREADS": "2"}
            for name in ("LD_PRELOAD", "FEX_ENV", "FEX_GDBSERVER", "STEAMVR_VRENV", "ARMADA_VR_XPRESENT_BOOTSTRAP"):
                env.pop(name, None)
            processes = []
            def interrupted(_signum, _frame):
                raise KeyboardInterrupt
            with (state / "steamvr-native.log").open("ab", buffering=0) as log:
                previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
                try:
                    def execute(command, timeout):
                        subprocess.run(command, env=env, cwd=binaries, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, check=True, timeout=timeout)
                    if args.native_preflight:
                        execute([str(bundle / "vulkan-interop"), "--direct-display"], 20)
                        print("Native direct-display capabilities advertised; rendering is not verified.", flush=True)
                        return 0
                    execute([str(bundle / "vulkan-interop")], 20)
                    execute([str(bundle / "vulkan-external-sync")], 20)
                    processes.append(subprocess.Popen([str(binaries / "vrserver"), "-keepalive"], env=env,
                        cwd=binaries, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT, start_new_session=True))
                    time.sleep(2)
                    if processes[0].poll() is not None:
                        raise RuntimeError("Native vrserver exited before device acceptance")
                    for mode in ("--setup-room", "--controllers"):
                        execute([str(bundle / "steamvr-probe"), str(binaries / "libopenvr_api.so"), mode], 45)
                    print("Native virtual devices and Vulkan sharing passed; rendering is not verified.", flush=True)
                    return 0
                finally:
                    for process in reversed(processes):
                        stop(process)
                    for path in sorted((directory / "logs").glob("*.txt")):
                        log.write(f"\nLOG_FILE={path.name}\n".encode())
                        with path.open("rb") as stream:
                            while chunk := stream.read(65536):
                                log.write(chunk)
                    for sig, handler in previous.items():
                        signal.signal(sig, handler)


def run(args):
    if args.native_devices or args.native_preflight:
        return run_native(args)
    if args.steam_client and (not args.virtual_display or args.probe):
        raise ValueError("Steam client launch requires the virtual display and cannot run with --probe")
    client = args.client.resolve()
    runtime = args.runtime.resolve() if args.runtime else client / "steamapps/common/SteamVR"
    validate_runtime_paths(runtime)
    binaries = runtime / "bin/linux64"
    fex_root = client / "steamapps/common/FEX-Emu/usr"
    fex = fex_root / "bin/FEX"
    required = [fex, *(binaries / name for name in ("vrserver", "vrcompositor", "vrmonitor")),
                args.icd, args.rootfs / "usr/lib/libvulkan.so.1",
                fex_root / "lib/aarch64-linux-gnu/fex-emu/HostThunks/libvulkan-host.so"]
    for path in required:
        if not path.is_file():
            raise ValueError(f"Missing session dependency: {path}")
    if args.probe and not args.probe.is_file():
        raise ValueError(f"Missing probe: {args.probe}")
    if args.resolver and not args.resolver.is_file():
        raise ValueError(f"Missing Vulkan resolver: {args.resolver}")
    if not args.render_node.exists():
        raise ValueError(f"Missing DRM render node: {args.render_node}")
    state = Path.home() / ".local/state/armada-vr"
    state.mkdir(parents=True, exist_ok=True)
    with (state / "steamvr-session.lock").open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise RuntimeError("An Armada VR session is already running") from error
        bundle = args.bundle.resolve()
        compositor_log = client / "logs/vrcompositor.txt"
        registry = None
        if args.virtual_display:
            registry, compositor_log = prepare_virtual_session(bundle, binaries, state)
            if args.resolver is None:
                args.resolver = bundle / "libvulkan-procaddr.so"
        config = state / "fex-session"
        config.mkdir(exist_ok=True)
        with fex.open("rb") as stream:
            fex_sha256 = hashlib.file_digest(stream, "sha256").hexdigest()
        if prepare_browser_config(config, fex_sha256):
            print("Applied browser-only cache-shrink guard for verified FEX-2607-76.", flush=True)
        (config / "Config.json").write_text(json.dumps({
            "Config": {"RootFS": str(args.rootfs),
                       "ThunkHostLibs": str(fex_root / "lib/aarch64-linux-gnu/fex-emu/HostThunks"),
                       "ThunkGuestLibs": str(fex_root / "share/fex-emu/GuestThunks"),
                       "SilentLog": "1", "OutputLog": "stderr"},
            "ThunksDB": {"Vulkan": 1}}) + "\n")
        overlays = sorted(str(path.relative_to(args.rootfs)) for path in
                          (args.rootfs / "usr/lib").glob("libvulkan.so*"))
        (config / "ThunksDB.json").write_text(json.dumps({"DB": {"Vulkan": {
            "Library": "libvulkan-guest.so", "Overlay": ["/" + path for path in overlays]}}}) + "\n")
        env = os.environ | {"FEX_APP_CONFIG_LOCATION": str(config) + "/", "FEX_PORTABLE": "1",
                           "VK_DRIVER_FILES": str(args.icd), "LVP_DRM_SYNC": str(args.render_node),
                           "LP_NUM_THREADS": "2"}
        env.update(prepare_fex_server(config, args.rootfs))
        env.pop("LD_LIBRARY_PATH", None)
        # SteamVR dereferences a null GPU when enumeration fails on older kernels.
        subprocess.run(["/usr/local/bin/vulkan-interop"], env=env, check=True, timeout=20)
        if args.resolver:
            env["FEX_ENV"] = "LD_PRELOAD=" + str(args.resolver.resolve())
        env.update({"STEAMVR_VRENV": str(binaries.parent / "vrenv.sh"),
                    "STEAMVR_TOOLSDIR": str(binaries.parent.parent),
                    "LD_LIBRARY_PATH": f"{binaries}:{binaries / 'qt/lib'}",
                    "VRCOMPOSITOR_LD_LIBRARY_PATH": f"{binaries}:{binaries / 'qt/lib'}"})
        if registry:
            env.update({"VR_PATHREG_OVERRIDE": str(registry), "MESA_VK_WSI_SW_PRESENT": "1",
                        "VR_OVERRIDE": str(binaries.parent.parent),
                        "VR_CONFIG_PATH": str(registry.parent / "config"),
                        "VR_LOG_PATH": str(registry.parent / "logs")})
        offset = compositor_log.stat().st_size if compositor_log.exists() else 0
        processes = []
        display_process = None
        display_directory = None
        session_directory = None
        def interrupted(_signum, _frame):
            raise KeyboardInterrupt
        previous = {sig: signal.signal(sig, interrupted) for sig in (signal.SIGINT, signal.SIGTERM)}
        try:
            with (state / "steamvr-session.log").open("ab", buffering=0) as log:
                session_directory = tempfile.TemporaryDirectory(prefix="session-", dir=state)
                directory = Path(session_directory.name)
                libraries = f"{binaries}:{binaries / 'qt/lib'}"
                if args.virtual_display:
                    runtime_libraries = prepare_runtime_libraries(bundle, client, directory)
                    libraries = f"{runtime_libraries}:{libraries}"
                env.update({"LD_LIBRARY_PATH": libraries, "VRCOMPOSITOR_LD_LIBRARY_PATH": libraries})
                if args.virtual_display:
                    display_directory = tempfile.TemporaryDirectory(prefix="display-", dir=state)
                    display_process, env = start_display(args.virtual_display, Path(display_directory.name), env, log)
                def launch(name, *arguments):
                    process_env = env.copy()
                    if name == "vrcompositor" and args.virtual_display:
                        compositor_libraries = f"{bundle / 'xpresent'}:{libraries}"
                        process_env.update({"LD_LIBRARY_PATH": compositor_libraries,
                            "VRCOMPOSITOR_LD_LIBRARY_PATH": compositor_libraries, "ARMADA_VR_XPRESENT_BOOTSTRAP": "1337"})
                    if name == "vrcompositor" and args.debug_compositor:
                        process_env["FEX_GDBSERVER"] = "1"
                    # SteamVR checks the compositor's Linux comm name for liveness.
                    interpreter = directory / name
                    interpreter.symlink_to(fex)
                    process = subprocess.Popen([str(interpreter), str(binaries / name), *arguments],
                                               env=process_env, cwd=binaries, stdout=log,
                                               stderr=subprocess.STDOUT, start_new_session=True,
                                               restore_signals=False)
                    processes.append(process)
                    return process
                launch("vrserver", "-keepalive")
                time.sleep(2)
                launch("vrcompositor", "-disablewatchdogs")
                print("Waiting for SteamVR's software GPU calibration and compositor startup...", flush=True)
                wait_for_startup(processes + ([display_process] if display_process else []),
                                 compositor_log, offset, args.startup_timeout)
                if args.probe:
                    for mode in ("--connect", "--setup-room", "--frames", "--controllers"):
                        print(f"Running SteamVR probe {mode}", flush=True)
                        subprocess.run([str(fex), str(args.probe.resolve()),
                                        str(binaries / "libopenvr_api.so"), mode],
                                       env=env, cwd=binaries, stdout=log, stderr=subprocess.STDOUT,
                                       timeout=190, check=True, restore_signals=False)
                    print("SteamVR frame probe passed.", flush=True)
                    return 0
                print("Compositor initialized; starting the SteamVR monitor.", flush=True)
                if args.virtual_display:
                    # A new virtual profile cannot reach Ready before room setup.
                    subprocess.run([str(fex), str(bundle / "steamvr-probe"),
                                    str(binaries / "libopenvr_api.so"), "--setup-room"],
                                   env=env, cwd=binaries, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=190, check=True, restore_signals=False)
                monitor_log = compositor_log.parent / "vrmonitor.txt"
                monitor_offset = monitor_log.stat().st_size if monitor_log.exists() else 0
                monitor = launch("vrmonitor", "-nokillprocess")
                if args.virtual_display:
                    wait_for_startup(processes, monitor_log, monitor_offset, 60,
                                     marker=b"to 'SteamVRSystemState_Ready'.")
                    subprocess.run([str(fex), str(bundle / "steamvr-probe"),
                                    str(binaries / "libopenvr_api.so"), "--dashboard"],
                                   env=env, cwd=binaries, stdout=log, stderr=subprocess.STDOUT,
                                   timeout=190, check=True, restore_signals=False)
                    print("Virtual SteamVR dashboard is visible.", flush=True)
                steam_client = None
                if args.steam_client:
                    command, client_env = prepare_steam_client(directory / "client", args.steam_client.resolve(),
                        fex, args.rootfs.resolve(), args.icd.resolve(), args.render_node, registry, runtime)
                    steam_client = subprocess.Popen(command, env=client_env, cwd=args.steam_client,
                        stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                    processes.append(steam_client)
                deadline = time.monotonic() + args.session_timeout
                while time.monotonic() < deadline:
                    if steam_client and steam_client.poll() is not None:
                        return steam_client.returncode
                    if monitor.poll() is not None:
                        return monitor.returncode
                    if any(process.poll() is not None for process in processes[:2]):
                        raise RuntimeError("SteamVR server or compositor exited during the session")
                    if display_process and display_process.poll() is not None:
                        raise RuntimeError("Virtual display exited during the session")
                    time.sleep(0.5)
                raise TimeoutError("SteamVR session reached its configured lifetime")
        finally:
            for process in reversed(processes):
                stop(process)
            if display_process:
                stop(display_process)
            if display_directory:
                display_directory.cleanup()
            if session_directory:
                session_directory.cleanup()
            for sig, handler in previous.items():
                signal.signal(sig, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, default=Path.home() / ".local/share/Steam")
    parser.add_argument("--runtime", type=Path,
                        help="Use a separate SteamVR installation; the compositor version check still applies")
    parser.add_argument("--steam-client", type=Path,
                        help="Launch this HOME profile's x86 Steam client after the virtual dashboard is ready")
    parser.add_argument("--rootfs", type=Path, default=Path("/usr/share/guestos/fex-mesa"))
    parser.add_argument("--icd", type=Path,
                        default=Path("/opt/armada-vr/mesa/share/vulkan/icd.d/lvp_icd.aarch64.json"))
    parser.add_argument("--render-node", type=Path, default=Path("/dev/dri/renderD128"))
    parser.add_argument("--startup-timeout", type=int, default=900)
    parser.add_argument("--session-timeout", type=int, default=3600)
    parser.add_argument("--probe", type=Path, help="Run the stereo acceptance probe after startup, then exit")
    parser.add_argument("--debug-compositor", action="store_true", help="Enable FEX's local guest debugger socket for the compositor")
    parser.add_argument("--resolver", type=Path, help="Load the guest-only Vulkan procedure lookup compatibility library")
    parser.add_argument("--virtual-display", choices=("headless", "window"),
                        help="Run the pinned SteamVR build with a private simulated headset and X server")
    parser.add_argument("--bundle", type=Path, default=Path(__file__).resolve().parent,
                        help="Directory produced by build-steamvr-probe.sh")
    native = parser.add_mutually_exclusive_group()
    native.add_argument("--native-devices", action="store_true", help="Test native ARM64 simulated devices and sharing without starting a compositor")
    native.add_argument("--native-preflight", action="store_true", help="Query native direct-display prerequisites without starting SteamVR")
    args = parser.parse_args()
    if not 1 <= args.startup_timeout <= 900 or not 1 <= args.session_timeout <= 14400:
        parser.error("Startup timeout must be 1–900 seconds and session timeout 1–14400 seconds")
    try:
        return run(args)
    except KeyboardInterrupt:
        return 130
    except (OSError, ValueError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
