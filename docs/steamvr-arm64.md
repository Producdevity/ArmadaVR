# Native ARM64 SteamVR runtime

For current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

Native tracking, composition and graphics remain the intended headset path.
The VM's x86-64 SteamVR through FEX is a compatibility baseline. It does not
establish native compositor portability or headset performance.
Valve documents Proton/FEX for Windows x86 games and forwarding graphics calls
to native libraries in its [Frame compatibility guide](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility?l=english).

## Native software rendering — October 10, 2026

The unmodified ARM64 server and compositor from Frame's SteamVR 2.17.10 now
render the stereo test in QEMU. The opt-in Lavapipe display backend supplies a real
XCB-backed `VK_KHR_display` surface and common WSI present-ID/wait support.
The compositor creates its swapchain and both distortion meshes, then accepts
120 stereo submissions with valid virtual poses. Captured 1024 × 512 output
shows the expected red left eye and blue right eye at both brightness levels.
Center pixels are `(64, 13, 5)` / `(5, 13, 64)` and
`(204, 13, 5)` / `(5, 13, 204)`, matching the submitted colors.

This test enables `enableLinuxVulkanAsync` to supply the compositor's compute
queue. Disabling `motionSmoothing` alone does not prevent Qualcomm optical-flow
initialization: the perception library aborts on QEMU's hardware identifier.
Omitting that optional library from a disposable VM copy exercises the
compositor's existing initialization-failure path, allowing rendering to proceed.
The acquired runtime and source disk remain unchanged. This is a virtual test
configuration, not a replacement for headset tracking or optical-flow support.

The investigation also found a Mesa 26.1.8 timeout bug. Its X11 present wait
compares the C11 condition-variable helper's result with POSIX `ETIMEDOUT`.
The helper returns `thrd_timedout`, so an ordinary deadline expiry incorrectly
becomes `VK_ERROR_DEVICE_LOST`. `patches/mesa/0003-x11-present-wait-timeout.patch`
corrects that comparison. A real presentation regression fails on the old code
after GPU completion and passes with the correction: all 12 pending waits
return `VK_TIMEOUT`, later waits complete, and the displayed pixels are correct.
It passes on ordinary XCB and twice on the virtual display, with no descriptor
leak or validation errors in those standalone tests.

The native compositor still exceeds its fixed 15 ms wait deadline with software
rendering. Its bundled validation layer also reports a device-feature-chain
duplication, a two-image swapchain below the reported minimum, descriptor-array
and acquire-semaphore errors, plus unknown private presentation structures.
These are separate from the standalone test's clean result. The virtual-display
backend has since been narrowed to FIFO presentation with a valid two-image
minimum; that removes the swapchain image-count validation error. The other
compositor diagnostics remain unresolved.
The later tests below add native dashboard interaction and Windows rendering.
Sustained timing, physical drivers and boot/recovery acceptance remain open;
no flash readiness follows from this software result. Evidence and negative comparisons are retained in
`output/native-wsi-followup-20261010-v1/` and
`output/frame-native-wsi-20261010-v1/` through `v14/`.

### Current official package comparison

Valve's [Frame hotfix repository](https://holo-packages.steamos.cloud/archlinux-deckard-hotfixes/)
now exposes individual ARM64 packages. The October 10 comparison uses
`deckard-steamvr-rel-r25839070+87334fb7-1`, downloaded and verified against the
official package database: 676,247,472 bytes, SHA-256
`3433c55c90c667b269842dd0b4cdf3c559ef7680f1a6f646269680725ca4e310`.
This avoids another complete recovery-image download. Its compositor hash is
`658991cbe9de1f6accc301dba101fdb1f8ee2109ffc812779397e5312a9bd525`.
The existing runtime and maintained profile remain unchanged.

In an isolated VM, the new runtime accepts 120 stereo submissions. Independent
captures verify both eyes at both brightness levels with the old bundled
validation layer and with Valve's current `1.4.363.0-4` validation package.
The observed compositor clock is 60.020 Hz. One earlier current-layer run
submitted every frame but missed the bright capture; it remains a failed
acceptance run. The later run adds the existing clock/focus observer, so its
success does not establish that the earlier failure is fixed.

Current validation retains the duplicate Vulkan 1.2/timeline feature chain,
descriptor-array count mismatch, unknown acquire/present structures and pending
acquire-semaphore errors. It also reports a presentation semaphore reused for
another swapchain image before reacquisition (`vkQueueSubmit` VUID `00067`).
Neither current Khronos headers nor the inspected public Valve headers define
the private presentation structure `1000002102`. No validation diagnostics are
suppressed and no private synchronization semantics are assumed.

The same driver passes the standalone display regression with current
validation: 32 clock events at 60.097 Hz, stable descriptor count, presentation,
surface lifecycle and refusal checks, with zero validation errors. This narrows
the remaining investigation to the compositor's API use and private contracts;
it does not establish that its synchronization or sustained frame timing is
correct. The new runtime has not passed the full Windows, Android and dashboard
acceptance matrix and is not promoted to the maintained profile.

Package provenance, successful and failed runs, independently captured pixels
and checksummed VM evidence are retained in
`output/compositor-validation-20261010-v1/`.

### Virtual-display regression

`patches/mesa/0004-lavapipe-virtual-display.patch` adds an explicit development
backend to Lavapipe. It exposes one 1024 × 512, 60 Hz display backed by an X11
window, with FIFO swapchains and common X11 present-ID/wait handling. Enable it
with `LVP_VIRTUAL_DISPLAY=1` and `MESA_VK_WSI_SW_PRESENT=1`. It requires an
authenticated X11 connection with Present and XFixes. Other surface platforms
are excluded from this opt-in mode; ordinary Lavapipe behavior remains the
default. This backend does not control a physical display.

On Linux with the patched ICD, an authenticated Xvfb display, Vulkan development
files, libxcb development files and `VK_LAYER_KHRONOS_validation` installed:

```sh
VK_DRIVER_FILES=/path/to/lvp_icd.aarch64.json \
LVP_VIRTUAL_DISPLAY=1 MESA_VK_WSI_SW_PRESENT=1 \
    just test-vulkan-display
```

The test uses the existing `DISPLAY` and `XAUTHORITY`. It verifies enumeration,
mode rejection, both capability queries, two-image FIFO presentation, pending
and completed waits, actual pixels, shared-surface ownership, recreation,
allocation-failure cleanup and 320 surface lifecycles across eight threads.
All owned windows and descriptors must be released, and standalone Vulkan
validation must remain clean. Each invocation creates a new output directory.

Native VM comparisons also verify refusal with the backend disabled, with
software Present disabled, without an X display and with XFixes disabled.
The current capability probe must be rebuilt: the historical base image's
older copy ignores `--direct-display`. Xvfb cannot disable its Present
extension at runtime; the unsupported-option attempt is retained as a failed
test setup. The maintained patch applies exactly to the source used for the
tested driver. The maintained regression compiles with warnings treated as
errors and passes on a fresh VM, including both virtual controllers and exact
stereo colors. All 223 host tests also pass. Final evidence is in
`output/frame-native-wsi-20261010-v22/`; an earlier capture of a fade transition
is retained with the corrected sampler comparison.

### Virtual display clock

The native compositor also needs `VK_EXT_display_control` and
`VK_EXT_display_surface_counter` in the virtual-display mode. Without them,
its fallback vblank thread increments the frame counter in an unpaced loop.
A baseline comparison measured about 8.3 million increments per second, and
57 of 59 Android predicted-display intervals collapsed to one nanosecond.

`patches/mesa/0006-lavapipe-virtual-display-clock.patch` supplies event fences
and a counter from actual X11 Present MSC notifications. It does not replace
missing events with immediate success or a guessed timer. The fixed virtual
display has no hotplug events; its hotplug fence remains unsignaled. Power
controls map or unmap its owned X11 window, including surfaces created while
powered off. Counter capability is limited to the owned virtual surface.
This remains an X11 software test backend, not physical display control.

The standalone regression covers event status and waits, wait-any/wait-all,
pending-fence destruction, hotplug timeout, power transitions and recreation,
ordinary XCB capability isolation, rendered pixels and descriptor cleanup.
Fresh native SteamVR backends with the final driver measure 60.011 and 59.996
counter increments per second for Android GLES and Vulkan respectively. Both
submit 60 frames, capture stereo color changes, restore private test settings
and exit normally. Neither has one-nanosecond predicted intervals. Software
rendering still misses compositor deadlines: median predicted-frame spacing is
about 50 ms, with 150 ms GLES and 84 ms Vulkan 95th percentiles in these bounded
runs. This establishes a clock correction, not frame-rate acceptance. Evidence and failed comparisons are
retained in `output/native-clock-followup-20261010-v1/`.

Windows HelloXR also passes with this driver on separate fresh native backends:
590 left-hand and 606 right-hand submissions, 108/119 haptic events, dashboard
focus transfer and restoration, visible trigger response, and normal menu-button
exit. The prefixes use separate writable overlays over an unchanged initialized
prefix. Both runs leave no VR, FEX or Wine processes. The service harness must
keep standard input open: HelloXR treats EOF as its quit key. Its earlier
pre-frame exit is retained as a failed harness comparison. All 233 host tests
pass; the captured Windows evidence is in the same clock-followup directory.

### Native dashboard startup

A fresh VM with the rebuilt maintained ARM64 device bundle now renders the
stock dashboard in both eyes. Captures show the Library welcome panel, toolbar
and controller laser. The test resolved three environment failures in sequence:

- The webhelper wrapper requires `xset`; without it, the wrapper reports an
  invalid X server even when the compositor can present.
- CEF needs NSS/NSPR and ALSA libraries, and the monitor needs its Qt/XCB
  platform dependencies.
- The shipped Qt 5.7 library lacks a `QPushButton::hitButton` symbol required by
  the monitor. Selecting the Frame image's system Qt 5.15.16 libraries first
  resolves it. The selected monitor and platform library pass a relocation
  check before startup.

The minimal VM init also left loopback down. A local TCP self-test failed with
`ENETUNREACH` before enabling `lo` and passed afterward. SteamVR's localhost UI
then became visible and rendered; the VM still had no external network adapter.
These dependencies and the Qt selection are confined to disposable test images;
the original runtime and Frame firmware remain unchanged.

Evidence is retained in `output/frame-native-wsi-20261010-v23/` through `v26/`,
including the missing-library, Qt-symbol and loopback failures. The maintained
bundle also repeats the exact stereo colors and both-controller input/haptic
checks. Native cached Library acceptance is recorded below. The monitor did
not emit the older runtime's Ready marker, so that marker is not counted as
passed. This result does not establish physical drivers or flash readiness.

### Native launcher and Windows applications

The maintained launcher provides an explicit software presentation mode:

```sh
python3 /path/to/native-bundle/steamvr-session.py --native-presentation \
    --runtime /path/to/disposable/SteamVR --bundle /path/to/native-bundle \
    --icd /path/to/lvp_icd.aarch64.json --native-qt /path/to/qt
```

The Qt directory must contain compatible ARM64 libraries under `lib/` and the
XCB platform plugin under `plugins/platforms/`. The X11, CEF and loopback
requirements above still apply. The default display is private Xvfb;
`--virtual-display window` selects Xephyr. To run the finite stereo submission
test instead of the dashboard, replace `--native-qt` with
`--probe /path/to/native-bundle/steamvr-probe`.

This mode verifies the Frame compositor checksum and the built bundle. It
requires a disposable runtime copy without the optional Qualcomm perception
library and never removes that library itself. Device-only checks retain their
original profile. Presentation enables asynchronous Vulkan and the application's
compositor initialization path in a temporary profile. Controller preflight stays
in `--native-devices`: running it before presentation can automatically start a
second compositor and dashboard, so presentation starts its own components
directly after room setup. Logs are retained in
`~/.local/state/armada-vr/steamvr-native.log`.

Windows HelloXR now renders both eyes through FEX and Proton while the server,
compositor and dashboard run directly as ARM64. Separate fresh backends and
prefixes pass left- and right-controller trigger input, haptics, dashboard focus,
return to the application and menu-button exit. The runs submit 403 and 400
frames respectively and both exit zero. Reviewed captures show the controller
cube changing size, the native Now Playing dashboard, and the original scene
restored after dismissal. Each hand's haptic counter increases from 0 to 75.

Two environment distinctions are required. `LVP_VIRTUAL_DISPLAY=1` belongs to
the compositor; Windows applications use `0` so Wine can request its ordinary
window-system extensions. `startCompositorFromAppLaunch` must be true for
OpenXR: disabling it skips client initialization even when the compositor is
already running, causing a null shared-state lock in the Frame client. The
native webhelper must also start explicitly because the delivered runtime has
no x86 webhelper at the path an x86 application attempts to launch.

The maintained Windows launcher now has an explicit `--native-bundle` mode;
see [Windows launch instructions](windows-openxr.md#launch-in-the-active-virtual-steamvr-session).
The original integration evidence and failed comparisons are retained in `output/frame-native-wsi-20261010-v30/`
through `v42/`, with reviewed acceptance records in `v41/` and `v42/`.
The maintained presentation launcher subsequently passes the right-hand Windows
run with 399 submissions, normal menu exit, and no SteamVR or X server processes
left after interrupting the launcher. The failed duplicate-startup comparison
is retained in `v45/`; the corrected run is in `v46/`.
Both maintained launchers then pass fresh right- and left-hand runs in `v48/`
and `v49/`: 385/375 submissions, 71/63 haptic events, focus transfer, reviewed
stereo/dashboard captures and normal menu exits. Rejected existing-prefix,
wrong-interpreter and wrong-backend launches preserve the prefix registries.
The private FEX configuration is removed; no translation, SteamVR or display
processes remain at the final check. All 233 host tests pass.
All original disks, firmware and runtime files remain unchanged. These are
bounded software tests, not headset performance or installation acceptance.

### Native Steam client Library

The ARM64 Steam client now renders its cached Library Home content through the
native backend. OpenVR reads back a 1920 × 1080 main texture and an 1800 × 120
toolbar, with game artwork confirmed in the captures. This uses an ephemeral
copy-on-write client profile over the preserved desktop disk, without an
external network adapter. It establishes cached content rendering, not a new
online sign-in or game execution.

The minimal test image needed `lsof` for Steam's local UI transport. Without it,
Steam displayed error `0x3009` and repeatedly failed its localhost WebSocket
handshakes. Adding the retained native utility restored both connections and
normal shutdown. `Containerfile.steam` already includes this dependency.

Library texture sharing requires the previously tested Zink path with patched
Lavapipe: `MESA_LOADER_DRIVER_OVERRIDE=zink`, `LIBGL_KOPPER_DRI2=true`, and
`LVP_VIRTUAL_DISPLAY=0` for the client. The ARM64 browser helper otherwise forces
`LIBGL_KOPPER_DISABLE=true`, preventing that path. The desktop build now allows
an explicit `LIBGL_KOPPER_DISABLE=false` override while preserving the default
when the variable is unset. The compositor retains its separate virtual-display
configuration.

The 256 MiB `/dev/shm` test mount was nearly exhausted during browser restarts.
A fresh comparison with a 512 MiB limit, within the same 3 GiB VM, renders both
textures with one observed local UI connection, no framebuffer errors or kernel
OOM report, and normal client shutdown. This bounded check does not establish
sustained stability. Evidence, original-file checksums and failed comparisons
are retained in `output/frame-native-wsi-20261010-v50/` through `v60/`.
The exact helper generated by the maintained build recipe also passes a fresh
run in `v62/`, including left-controller menu activation and normal shutdown.
The helper's unset/default and explicit-override cases pass separately, as do
the three desktop launcher tests.

Longer Library tests exceeded the 3 GiB guest's available memory and the kernel
killed the browser. Moving diagnostic captures to the guest disk reduced memory
use but did not prevent that failure. A 1 GiB swap file in the disposable guest
snapshot allows the bounded journey to complete without an OOM report; no host
swap setting or original disk is changed. This is a test-resource requirement,
not a demonstrated headset memory budget.

Both virtual controllers now select Library, scroll the rendered game grid and
navigate to Home. Read-only browser diagnostics confirm the actual scroll
position and loaded artwork; all navigation and scrolling still come from the
controllers. Three large swipes continue moving thousands of pixels after
release, so early captures show placeholders. A smaller single swipe and
measured settling produce rendered content with both hands. The test rejects a
smaller gesture that produces no scroll movement instead of counting input
delivery as success. These comparisons are preserved in `v63/` through `v69/`.

A fresh repeat in `v70/` verifies the complete bounded journey: left and right
gestures move the grid by 457 and 521 CSS pixels, each hand returns it to exactly
zero, and both select Home and Library. Reviewed overlay captures confirm the
game artwork and resulting pages. Steam exits normally, the backend leaves no
owned processes, and the VM powers off with all original hashes unchanged. No
kernel OOM or framebuffer-incomplete error is reported in that run.

The native client and browser execute as ARM64 and map the tested native
Lavapipe driver. Normal Steam shutdown and scoped backend cleanup pass. This
establishes cached offline Library interaction; online sign-in, game launch,
sustained stability and physical controller behavior require separate tests.

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

The simulated headset profile selects a controller pose resource owned by the
virtual-driver bundle. It defines tip and grip transforms at the simulated device
origin; it does not supply a physical controller mesh. Frame omits the older Vive
render model used by the translated test profile, so relying on that model leaves
native controller-tip queries invalid. The bundle builder includes and hashes the
pose resource, and native session validation requires it. The native controller
probe checks both hands' actual OpenVR tip and grip transforms before testing
input and haptics.

Fresh native VM tests verify both hands opening the actual Settings panel and
returning to the Library welcome panel. The test measures the runtime's toolbar
transform, aims the simulated controller ray at the button, presses and releases
its trigger, and restores both controllers afterward. Captures from the
compositor show each navigation result, and the primary pointer changes to the
selected hand. The missing-model comparison and fixed component checks are in
`output/frame-native-wsi-20261010-v27/` and `v28/`; reviewed interaction evidence
is in `v29/`. All 224 host tests pass. This proves stock dashboard navigation,
not authenticated Library contents, physical tracking or native headset timing.

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

October 8 tests extend the native graphics evidence without starting a
compositor: `tests/vulkan-external-image.c` passes real opaque-FD image
export/import across fresh exec processes, external queue ownership transfer
and all 65,536 pixel checks in 16 cases. The maintained fixture also passes
as UID 1000 on the desktop Quest-kernel VM; FD counts return to 3 each time.
See [image-sharing acceptance](steamvr.md). This does not establish concurrent
image synchronization, SteamVR image-manager IPC or presentation.

A separate temporary driver exposes OpenVR's driver-direct component and
reports `HasDriverDirectMode=true`, but Frame's compositor still constructs
its Vulkan WSI window before invoking the driver's swapchain/presentation
callbacks. That route does not bypass the demonstrated display/present-wait
requirements. The original Valve runtime remains unchanged.

The same image supplies host Android graphics overlays but no matching Lepton
rootfs/sysbake. A separate authenticated Steam installation now supplies that
payload. [Actual Android boot, Binder services, context persistence, software
APK rendering and network provisioning pass](android-containers.md#acquired-lepton-and-actual-android-boot)
on the Quest test kernel. Android OpenXR remains unverified.

## Translated Steam client startup comparison

The public x86 beta client build `1791415817` now reaches its real CEF sign-in
window in a clean profile on the same Quest-kernel desktop VM. No account or
session files were copied into that profile. This is client/container startup
proof; authenticated Library navigation and its SteamVR websocket remain open.
The native ARM64 client and compositor remain the intended headset architecture.

The initial generic user-namespace dialog was not a missing kernel feature.
A real translated bubblewrap namespace/bind test passes. The delivered
pressure-vessel `0.20260824.0` requirements checker fails with its default helper
but passes with a verified native ARM64 bubblewrap selected through
[`PRESSURE_VESSEL_BWRAP`](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/9c2a654cbd94ce62aa93de899af92ed11ed36bc2/steam-runtime-tools/bwrap.c#L220).
Use a distinct absolute native-helper path: FEX can otherwise resolve the same
`/usr/bin/bwrap` name to the x86 rootfs copy.

The complete delivered `steam.sh` also needs the original Valve FEX
`emulator.json` selected through
[`STEAM_COMPAT_EMULATOR`](https://gitlab.steamos.cloud/steamrt/steam-runtime-tools/-/blob/9c2a654cbd94ce62aa93de899af92ed11ed36bc2/pressure-vessel/wrap-context.c#L835).
Its actual server manager and compatibility launcher establish the FEX server
and container environment. Direct client execution or relying on the generic
host binfmt interpreter fails that integration. The successful diagnostic uses
`STEAM_FORCE_CLIENT=steamrt64` and a private FEX configuration, and does not skip
the requirements check or alter global namespace policy. Its finite timeout
ends the client by SIGTERM; normal user exit and performance are unverified.

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
