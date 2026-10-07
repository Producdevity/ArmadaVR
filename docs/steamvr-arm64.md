# Native ARM64 SteamVR runtime

For current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

Native tracking, composition and graphics remain the intended headset path.
The VM's x86-64 SteamVR through FEX is a compatibility baseline. It does not
establish native compositor portability or headset performance.
Valve documents Proton/FEX for Windows x86 games and forwarding graphics calls
to native libraries in its [Frame compatibility guide](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility?l=english).

## Official Frame runtime obtained — October 7, 2026

Valve's [official repair image](https://steamdeck-images.steamos.cloud/recovery/steamframe-oobe-repair-20260922.5153644-0.3.0.img.zip)
now provides an obtainable native runtime. Offline inspection of its `rootfs-A`
found `/opt/steamvr`, package `deckard-steamvr-rel` version
`r25358740+28b72a4f-1`, with build marker `1789606310`. The complete runtime was
retained privately, with a file hash inventory. Its license is recorded as
`LicenseRef-steam-subscriber-agreement`; redistribution has not been reviewed.

| Component | ELF / userspace ABI |
|---|---|
| `bin/linuxarm64/vrserver`, `vrcompositor`, `vrmonitor`, `vrstartup`, `vrpathreg` | AArch64 Linux/glibc |
| `bin/linuxarm64/vrclient.so` | AArch64 Linux/glibc |
| `bin/androidarm64/vrclient.so` | AArch64 Android/Bionic; depends on Android GLES/EGL, log and libc |
| `bin/linux64/vrclient.so` | x86-64 Linux/glibc application compatibility client |
| `steamxr_linuxarm64.json` | Selects `bin/linuxarm64/vrclient.so` |

The ZIP SHA-256 is
`672df1b9c5c6b849df506c995a32fe136b53c4db688f924e6727639dd8b963a9`.
The whole-image ZIP CRC, primary GPT header/table CRCs, partition bounds and
extracted partition hash were checked. Official HTTPS provenance and these
integrity checks do not establish manufacturer-signature authentication.
No image was mounted, booted or supplied to a headset.

The five Linux executables' direct library dependencies resolve in an isolated
ARM64 Fedora 44 container after supplying Debian Bookworm's `libpcre16-3` for
the bundled Qt libraries. The native `vrpathreg show` command executes and reads
the isolated runtime registry. This is loader/CLI evidence, not a compositor,
dashboard, OpenXR rendering or Quest-kernel pass.

Two integration details need explicit handling:

- The shipped `bin/vrenv.sh` still selects `linux64`. Native launch must select
  `linuxarm64` and its library paths; blindly invoking that wrapper would select
  the x86 runtime.
- The package has an absolute PulseAudio compatibility-library link outside
  its runtime tree. Preserve the original package and validate the chosen host
  library set; do not rename unrelated libraries to satisfy a SONAME.

Frame's service launches native tools after Gamescope and includes Frame-specific
display/charger hooks. Its hardware and computer-vision components are not Quest
drivers. The next runtime milestone is a separately validated native session in
QEMU, including ARM64 virtual-driver/probe builds, software graphics sharing,
both-hand dashboard interaction and Windows rendering on a fresh backend.
Keep the existing translated runtime and its evidence for comparison.

## Native virtual device acceptance

The Frame package omits Valve's null-headset driver. Armada VR's existing virtual
controller driver now optionally supplies a fixed simulated HMD when the selected
driver is `armada_virtual` and `driver_armada_virtual.simulateHeadset` is explicitly
true. The original null-HMD/x86 path remains available. Neither simulated path
has a physical-device transport.

Build the native probe and driver in an AArch64 Linux development environment with
Clang, Vulkan headers/loader and the pinned OpenVR headers:

```sh
just build-steamvr-probe output/openvr-sdk output/steamvr-native-probe aarch64
```

The native bundle contains `armada_virtual/bin/linuxarm64/driver_armada_virtual.so`,
the native virtual profile, session launcher and Vulkan sharing probes. On
unprivileged AArch64 Linux, select the locally supplied runtime explicitly:

```sh
python3 output/steamvr-native-probe/steamvr-session.py \
  --bundle output/steamvr-native-probe --runtime /path/to/frame/steamvr \
  --icd /path/to/verified/lavapipe.json --render-node /dev/dri/renderD128 \
  --native-devices

python3 output/steamvr-native-probe/steamvr-session.py \
  --bundle output/steamvr-native-probe --runtime /path/to/frame/steamvr \
  --icd /path/to/verified/lavapipe.json --render-node /dev/dri/renderD128 \
  --native-preflight
```

`--native-devices` checks the bundle hashes, ARM64 runtime ABI and simulated
profile, then uses a private OpenVR registry with automatic component launch
disabled. It runs real cross-process buffer/semaphore sharing and native server,
HMD/controller acceptance; it does not launch a compositor or dashboard. Native
components are invoked directly without FEX or Frame's x86-oriented `vrenv.sh`.
The session lock is shared with the existing translated launcher. Owned server
processes and private session files are cleaned up; logs remain in
`~/.local/state/armada-vr/steamvr-native.log`.

`--native-preflight` queries display, present-id/wait features and semaphore
capabilities on the same Vulkan device, without launching SteamVR. A successful
query still requires display acquisition, scanout and compositor acceptance.
The VM backend currently fails this query and the launcher exits unsuccessfully.

SteamVR **2.17.10** now executes on the unchanged Quest-derived **5.10.246 QEMU
transport kernel**, as UID1000 in a new snapshot-only filesystem. The native probe
passes room setup, both controller poses, press/release for trigger/grip/menu/trackpad,
input-origin checks, malformed-request rejection and haptic delivery. The existing
patched Lavapipe also passes binary/timeline semaphore plus 4 MiB buffer round trips
across processes in this VM. These are virtual-device and sharing checks, not
physical tracking or rendering proof.

Two consecutive maintained native device sessions on a fresh VM pass, including
cleanup and restart after the expected graphics refusal. A separate fresh native
compositor test under authenticated Xvfb fails with SIGSEGV
while looking for a direct display through Vulkan WSI. Disabling `direct_mode.enable`
reaches the same failure. GDB confirms an indirect call to address zero at
compositor offset `0xb39a8`, returning to `0xb39ac`. Runtime lookup tracing and
disassembly identify `vkGetPhysicalDeviceDisplayPropertiesKHR`: the compositor
calls the null pointer even though `VK_KHR_display` is not enabled. The
[Vulkan lookup contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetInstanceProcAddr.html)
permits a null pointer for an unavailable extension command.

A debugger-only experiment that returns an empty display list gets past that
call, then reports missing `VK_KHR_present_wait` support and aborts in a later
Vulkan device call. No runtime binary was changed. Valve's simulated-display
debug property and scanout-scaling settings also fail to select a working window
path. These failures require a compatible native presentation backend; adding a
null-call workaround alone does not provide one. Preserve the failed runs.
Native compositor presentation, stock dashboard interaction and Windows
rendering on this backend remain open.
The QEMU runtime has no network, host filesystem sharing or passed-through devices.

The same image supplies host Android graphics overlays but no matching Lepton
rootfs/sysbake. [Podman's three writable views now pass on the unchanged Quest
test kernel](android-containers.md#podman-writable-mount-acceptance); actual
Android startup still requires the patched Lepton payload.

## Earlier search results

On September 11, 2026, the following checks found no obtainable complete ARM64
SteamVR runtime. This is a bounded search result, not proof that none exists.

| Source | Verified result |
|---|---|
| [Valve OpenVR SDK](https://github.com/ValveSoftware/openvr/tree/0924064316de3effbcd1acf1e309182a2deb1c05) | Linux/Android ARM64 API loaders are present. The complete tree contains no `vrclient.so`, `vrserver` or `vrcompositor`. |
| [Holo ARM64 preview](https://gitlab.steamos.cloud/holo/holo-core-aarch64-preview) | The public `mash-20251118.3` package databases contain 254 core and 4,306 extra packages; no SteamVR/Deckard package match was found. |
| [SteamOS package index](https://steamdeck-packages.steamos.cloud/archlinux-mirror/) | The inspected Holo main/release repositories expose x86-64 directories. The 178-package Holo main database has no SteamVR/Deckard match. |
| [Public recovery index](https://steamdeck-images.steamos.cloud/recovery/) | Steam Deck images are listed; no Frame recovery image was found. |
| Public GitHub/GitLab searches and unofficial reports | API-loader mirrors and historical ARM Lighthouse references did not provide a complete runtime. |
| Restricted sources | The Holo package root returns HTTP 401. Anonymous access to Lepton app 3029110/depot 3029111 is denied; an earlier SteamVR DLC 4128110 attempt was also denied. No content was obtained from these restricted sources. |

An [independent Lepton inspection](https://utzcoz.github.io/2026/09/03/steam-frame-lepton-architecture.html)
reports `deckard-steamvr-main`/`deckard-steamvr-rel` package names and an Android
`/data/steamvr/runtime/bin/androidarm64/vrclient.so` path. The author explicitly
says the runtime binary was unavailable for inspection. These are leads; the
underlying scripts and runtime were not independently obtained here.

The existing ARM64 Steam client, Proton, OpenVR SDK loaders, Monado and ALVR
are distinct components. None supplies the missing Valve ARM64 runtime merely
by being ARM64. The ARM64 Wine OpenXR bridge cannot load the older desktop test
runtime's x86-64 `vrclient.so`; the historical ELF evidence is in
`output/steamvr-runtime-architectures/report.json`.

Exact URLs, response statuses, package metadata, hashes and search limits are
preserved in `output/steamvr-arm64-research-v1/report.json`. A newly obtainable
runtime should first be inspected for architecture, matching client/server ABI,
dependencies and provenance, then tested in an isolated VM session. The current
validated x86 runtime and its evidence must remain available for comparison.
