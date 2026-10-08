# SteamVR in the development VM

The Steam-managed copy updated to beta 2.17.9 after the September 10 reboot.
Its compositor fails the 2.16.7 checksum guard; a separate beta experiment also
crashed. The successful results below belong to 2.16.7. Use the launcher's
`--runtime /path/to/verified/SteamVR` option to test a separate pinned runtime
while `--client` continues to locate Steam's FEX and runtime dependencies.
The compositor checksum check remains enabled. The restored public runtime also
crashed during interactive startup after this reboot; the new regression is not
specific to the beta. An undebugged frame/controller repeat passes with 192
actual presentations. A subsequent clean session restores native OpenXR and
Windows sample rendering; repeated Windows session stability remains unresolved.
The separately restored runtime must keep relative paths inside its own tree;
symlinking its OpenXR manifest to the managed installation selected the beta
client unexpectedly. The current interaction and application results are recorded below.

The isolated public SteamVR 2.16.7 test now passes actual application presentation
and simulated controller acceptance. Its 120 stereo submissions produce 147
compositor presentations attributed to the test process; both controllers pass
pose changes, button press/release and haptic delivery. The visible nested-display
run also passes with 101 presentations and both controllers. Eleven captures
verify more than 80,000 red/blue pixels per eye and both expected brightness
levels, matching the submitted Vulkan colors. The maintained launcher also
passes on the rebuilt kernel with 143 presentations and both controller tests.
The monitor now reaches `Ready`, then `Standby`, after correcting the translated
compositor's Linux process name. The real library dashboard renders in both eyes,
and a fresh launch opens it automatically. This is
software-rendered virtual-headset proof, not physical headset or VR game proof.

The validated runtime is SteamVR 2.16.7 (public build 23791826), with Valve's
FEX-Emu compatibility tool and Steam Linux Runtime Sniper. SteamVR's Linux
server, compositor and monitor are x86-64. The ARM64 Steam client launches
these through FEX. The pinned client download now also includes the x86 Steam
SDK libraries, Scout and the ARM64 runtime launcher service.

The [native ARM64 runtime investigation](steamvr-arm64.md) subsequently obtained
SteamVR 2.17.10 from Valve's Frame repair image. Native virtual devices and Vulkan
sharing pass; compositor presentation remains blocked by the VM's missing direct
display and present-wait support. The translated desktop runtime remains a VM
test baseline.

The FEX graphics-provider root filesystem is the verified Arch Linux SquashFS
from `profiles/vr-runtime.json`, mounted read-only at
`/usr/share/guestos/fex-mesa`. That installation, Steam's downloaded dependencies
and the guest swap file currently live in the development overlay. A fresh
desktop build does not yet reproduce all these additions.

## Fresh backend and stock input, October 8

The archived desktop disks were restored with their original lengths and hashes;
a new child overlay holds every new guest write. A reconstructed, archive-verified
x86 Proton disk is mounted read-only. No network, USB or host shares are attached.
The installed bundle and all 3,544 public-runtime files pass integrity checks.

Fresh backend v18 did not reach headset readiness. An experimental Frame 2.17.10
ARM64 client also fails path-manager initialization against this 2.16.7 server;
these versions must not be mixed. Backend v19, without that mixed client, reaches
compositor readiness and the stock dashboard. The failed run remains preserved;
this comparison does not establish its startup failure's cause.

Both controllers now intersect the actual `system.systemui` overlay, select the
primary pointer with their triggers, and close/reopen the stock dashboard using
250 ms system-button presses. The earlier one-second stimulus overlaps the stock
binding's one-second recenter threshold: a failed left-hand Windows run records
`Recenter action triggered` without the requested dashboard transition. A separate
stock test also coincides with a system UI renderer restart. Short presses pass
for both hands without changing the controller driver or Valve bindings.

The actual overlay uses an atlas: its measured pixel-coordinate transform and
Vive pointer-tip transform determine the tested controller pose. Frontend scene
keys such as `system.dashboard.quicklaunch` are not public overlay handles.
The stock Library still displays the launch-Steam welcome panel and reports its
separate Steam websocket disconnected. Successful pointer/toggle tests do not
establish Library navigation or rendered scrolling; renderer reliability remains
open. Windows rendering and focus pass for both hands on v19 and another fresh backend
v20; one intervening unexpected process exit remains unresolved. Results are in
[Windows OpenXR](windows-openxr.md).
Evidence is in `output/desktop-backend-20261008/`.

An additional fresh backend v26 passes Windows right/left focus, rendering,
haptics and normal menu exit. It also reproduces two web-helper zygote SIGTRAPs;
one attempt fails when a transient core dump exhausts guest storage. The
available compressed core and both metadata records remain private evidence.
Resource limits contain future dump growth but do not resolve the browser crash.
All public-runtime and installed-bundle files still pass integrity checks.
See [the later repeat and its limits](windows-openxr.md#fresh-backend-repeat-october-8).

The packaged interpreter is `FEX-2607-76-g37265b1`, with SHA-256
`f9cbb7a70e804b9930fed02fd9d1349d49d7808faace703063a9b4944bc3568e`.
It predates [FEX's L1 cache invalidation fix](https://github.com/FEX-Emu/FEX/pull/5856).
A real growth/rewrite/shrink regression executes stale code with its default
settings and executes the rewritten code with cache shrinking disabled. The
same regression passes using the maintained launcher's private per-application
configuration, with the test executable named `vrwebhelper`.

For this exact interpreter hash, `steamvr-session.py` creates
`fex-session/AppConfig/vrwebhelper.json` with
`DynamicL1CacheDecreaseCountHeuristic` set to `"0"`. Compatible existing files
are preserved; conflicting files, symbolic links and inherited conflicting
environment settings are refused. Other interpreter builds are left alone.
The setting applies to the translated browser helper; games and the compositor
retain their existing configuration. Disabling shrinking can retain more cache
memory, and headset performance is untested.

This proves the stale-code guard, not the cause or resolution of the CEF failure.
[Upstream reports](https://github.com/FEX-Emu/FEX/issues/5336) retain browser
failures after the invalidation fix alone. Fresh cache-test backend v2 passes
left- and right-hand Windows focus/haptic/menu exits. A later left-hand run
fails when the backend's configured deadline interrupts its stimulus; the
failure is preserved. No new core dump is recorded during that session.
The cache comparison and all three results are retained in
`output/blockers-20261008-v2/cef-cache-v2.tar.gz`.

## FEX session isolation

The translated launcher now gives each configuration/rootfs pair its own FEX
server data directory and socket namespace under the user's runtime directory.
An existing FEX server supplies its rootfs to connecting clients, even when
their configuration selects another rootfs. A real VM regression reproduces
that interference with a live translated process; the maintained isolation
helper rejects the deliberately missing rootfs while the valid session keeps
working. Linked/shared directories and paths exceeding the Unix socket limit
are refused, avoiding FEX's fallback to a shared socket.

Steam's compatibility tool also rewrites its client configuration with a
temporary graphics-provider path. Restart tests must regenerate their initial
configuration from the verified inputs. A separately scoped x86 client now
authenticates using an explicitly authorized guest-only session copy alongside
a fresh SteamVR dashboard. In that separately configured profile, its stock
Library websocket remained disconnected; authentication alone did not establish
Library navigation or scrolling.

## New profile startup and authenticated client

A new virtual profile previously timed out waiting for the monitor's `Ready`
state while the monitor requested room setup. The launcher now commits the
simulated room after compositor initialization and before waiting for the
monitor. The same previously unconfigured profile reaches `Ready` with the
corrected launcher. This applies only to the validated virtual headset path.
All 159 host tests and the rebuilt native bundle's source/artifact checks pass;
native compositor presentation remains unverified.

The authorized guest-only Steam-session copy authenticates, and the original
session inputs remain unchanged. Both the delivered client's OpenVR API and
the pinned runtime API initialize successfully inside the client's actual
mount/root namespace after backend readiness. Early probes fail before
initialization or after the backend deadline; those failures remain preserved.
Autostarting dashboard executables from the API probe's environment also reports
missing libraries. API initialization alone does not prove Steam's own dashboard
startup or its authenticated Library connection.

The isolated installation record initially used an obsolete depot section.
Steam normalized it to an empty `InstalledDepots` section and reported SteamVR
not installed. A private repair uses the three actual verified depot manifests,
their file sizes and an installation timestamp; all 3,544 files remain unchanged
and read-only. Steam still reports installation and update errors for this pinned
runtime. A matched HOME profile then exchanges SteamVR capabilities,
initializes actions and establishes the actual Steam websocket connection.
Steam's VR window reports a first paint, followed by repeated renderer restarts
and an early client/backend exit. Guest teardown also reports bad page-cache
entries in a llvmpipe thread; the cause remains unresolved. Library navigation,
rendered scrolling and reliability are unverified. Raw login/session files
and their preservation snapshots stay inside the isolated guest.

## Dashboard interaction, September 11

The newer backend v17 uses the unchanged maintained launcher from bundle v10,
and both pre/post checks pass all 3,544 runtime files. Its software GPU startup
takes 667.45 seconds. The bundle's Windows test passes, but dashboard diagnostics
v9/v10 initially fail close/reopen. The system UI web helper restarts at
17:45:48 and 17:54:36 UTC; the second restart coincides with v10's right-hand
close failure. The server/compositor remain alive, with zero memory/pid-limit
events and zero OOMs. The cause of the renderer exit is not yet established.

Two later repetitions of v10 pass both hands' pointing, primary selection,
click, three smooth-scroll swipes, menu/Back events and close/reopen. The first
captures process signals with strace; the second only captures logs. Neither
restarts the renderer, and neither sees nonfinite scrolling. Handled signals
in the translated process are not themselves evidence of a crash. These passes
do not resolve the earlier intermittent failure or prove stock Library
navigation/rendered scrolling. Continuous logs, failed tests and final counters
are preserved in `output/steamvr-backend-v17/`.

The stock Library still shows the launch-Steam welcome panel. The server reports
a Steam connection, while the dashboard reports its separate Steam websocket
disconnected. Stock Now Playing renders over the running Windows sample.
The native client's missing ARM64 `vrclient.so` and `-vrdisable` workaround
remain relevant to this connection boundary; their role here is not yet proven.

Backend v16 uses the verified public 2.16.7 runtime on the QEMU ABI-v7 kernel.
The development Proton disk is mounted read-only; all 3,544 runtime files pass
the preparation hashes. Frame/controller acceptance passes before the separate
interaction tests.

The dashboard diagnostic measures the overlay's pixel-coordinate transform,
the Vive render model's pointer-tip transform and the actual controller pose.
Both hands hit the panel center at UV `(0.5, 0.5)`, become primary, and deliver
mouse press/release, discrete scrolling, application-menu events (button 1),
Back events (button 2), and system-button close/reopen. The passing event record
is `output/steamvr-dashboard-v8/diagnostic.log`.

Earlier failures are retained: v5 queried the panel before placement and used
normalized coordinates where pixel coordinates were required; v6 still missed
short input pulses. Longer pulses and waits in v7 resolve both-hand selection.
Home is delivered to the overlay as a button event; it does not automatically
change an application's page. The v7 smooth-scroll stream also includes NaNs.
The discrete-scroll test passes without nonfinite values, but the smooth-scroll
issue remains unresolved. No controller driver change was required for these
results. Full stock Library navigation and rendered scroll-content behavior
remain separate from this diagnostic overlay's received events.

Windows samples also verify both-hand dashboard/application focus transfer,
menu-to-quit and virtual haptic delivery; see [Windows OpenXR](windows-openxr.md).
These results do not establish physical tracking, input mapping or flash readiness.

## Initial presentation timing

`output/steamvr-bootstrap-1337-v1/` preserves the passing run, configuration,
source and checksums. Startup takes 814.4 seconds under software rendering.
Read-only inspection of the exact public compositor shows an initial expected
XPresent serial of **1337**. It waits for that serial before requesting subsequent
timing events. The earlier serial-zero experiment below never matched it.

Requesting one real `XPresentNotifyMSC` with serial 1337 after selecting input
and flushing the request lets the loop advance. The server generates all timing
and pixmap-completion events; no frame statistics or completion events are
fabricated. The test uses a private Xvfb server whose UST/MSC clock advances.
The main desktop's modesetting server instead returns zero timing values despite
its nominal 75 Hz RandR mode; an earlier run there displayed the real dashboard
but rejected the application texture with error 105. These are distinct results.

The maintained source patch is opt-in and the launcher checks the compositor
SHA-256 before enabling its version-specific initial serial. A private OpenVR
registry selects only the virtual null headset and virtual controllers. The
normal Steam installation and user configuration are not patched. Independent
VM DMA-buffer-export work is still useful, but missing DMA-buffer export was
not the cause of the passing test's original presentation stall.

The visible run takes 602.9 seconds to initialize. Its first capture occurs
during fade-in; the subsequent eleven pass exact center-pixel values
`(64,13,5)` / `(5,13,64)` and `(204,13,5)` / `(5,13,204)` for the left/right
eye pulse. The captures, statistics and checker output are preserved in
`output/steamvr-nested-v1/`.

## Reproduce the virtual session

Run these commands inside the ARM64 development VM with the project sources,
installed SteamVR/FEX tool and the pinned FEX rootfs available:

```sh
bash tools/build-steamvr-probe.sh output/openvr-sdk output/steamvr-probe
python3 -B output/steamvr-probe/steamvr-session.py \
  --virtual-display window --probe output/steamvr-probe/steamvr-probe
```

The default ICD is the patched Mesa build at
`/opt/armada-vr/mesa/share/vulkan/icd.d/lvp_icd.aarch64.json`; use `--icd` for
another verified build. Use `--virtual-display headless` for Xvfb, or omit
`--probe` to start the monitor after calibration. Window mode needs Xephyr;
building needs `patch`, Clang/LLD and the pinned rootfs toolchain. The source
build fetches checksum-pinned OpenVR headers and XPresent source and preserves
both licenses in the bundle. It also builds the x86-64 OpenXR resolver and
includes the Windows OpenXR launcher; see [Windows OpenXR](windows-openxr.md).

The persistent development VM now has the verified bundle installed under
`/opt/armada-vr/openvr`, and the patched Mesa library at its default location.
Its **SteamVR** desktop menu uses `tools/desktop-steamvr.sh`. Current installation
hashes are recorded in `output/steamvr-bundle-v10/install-v7.json`. The previous
bundle is retained in `/var/tmp/steamvr-before-install-v7` inside the guest. No Steam login
files or Valve runtime binaries are replaced by this installation.

The virtual controllers expose the Vive dashboard's trigger, grip, system and
application-menu buttons plus trackpad axes, click and touch. Acceptance checks
the new menu/trackpad values and their release for each hand, verifies action
origins, moves the poses and observes haptic delivery. The current run passes
107 actual application presentations and both hands' expanded action tests;
evidence is in `output/steamvr-acceptance-v10/`. This fixes the missing input
components logged by the compositor. Dashboard navigation still requires its
own interactive test; application action polling while the dashboard has focus
does not establish a controller failure.

## Translated process names and dashboard dependencies

The original monitor reported error −203 even while the compositor accepted
connections and presented application frames. Valve describes that error as
[`StatusError_CompositorNotRunning`](https://github.com/ValveSoftware/SteamVR-for-Linux/blob/master/ErrorCodes.md).
The public `vrserver` binary checks `/proc/<pid>/comm` for `vrcompositor`; the
direct FEX launch instead names that process `FEX`. Launching the same interpreter
through a private symlink named `vrcompositor` preserves the actual component
name. It does not alter Valve binaries or override readiness results.

The name-only VM run initializes in 554.45 seconds and reaches the monitor's
`Ready` state without the −203 alert. Its screenshot and server/monitor logs are
under `output/steamvr-interactive-v3/`. Idle virtual-headset standby remains
normal; this does not establish that the dashboard web content renders.

The remaining missing NSS and legacy Qt SSL dependencies exist in the installed
Valve Sniper/Scout runtimes. `profiles/steamvr-runtime.json` pins fifteen library
links by relative source path and SHA-256. The launcher validates x86-64 ELF
architecture and every checksum before creating its private library directory.
It does not add either runtime's whole library tree or replace the Arch rootfs
libraries. Both monitor and web-helper ELF dependency checks pass. The full
interactive run also passes: 94 application presentations, 120 stereo submissions,
both controller action/haptic tests, and a captured stereo library dashboard.
Its evidence is in `output/steamvr-interactive-v4/`. A subsequent fresh launch
automatically waits for monitor readiness, configures the virtual room and opens
the dashboard. Its screenshot and exact launcher are in
`output/steamvr-interactive-v5/`. Neither run triggered a cgroup OOM or memory-limit
event. Startup calibration can take nearly fifteen minutes with software Vulkan.

The probe's `--dashboard` mode requests the dashboard and checks its visibility
within sixty seconds. This is an API check; a capture is still needed to verify
rendered dashboard content. Both interactive runs include that separate capture.
The displayed OpenXR-default notice is expected: the native ARM64 OpenXR tests
still select Monado explicitly. The x86-64 SteamVR runtime is a separate test path.

## OpenXR application acceptance

The maintained sample in `tests/steamvr-openxr.cpp` uses Khronos' official
OpenXR loader and the installed SteamVR runtime. It checks the runtime identity,
valid stereo poses, 120 projection layers, orderly session exit and nonzero
actual compositor presentations attributed to its own PID. Separate display
captures verify eye colors. The shared renderer negotiates RGBA/BGRA UNORM or
SRGB swapchain formats because this SteamVR runtime advertises SRGB.

Inside the prepared ARM64 Linux VM:

```sh
just build-steamvr-openxr /path/to/openxr-sdk-b5fd54b.tar.gz output/steamvr-openxr
just test-steamvr-openxr output/steamvr-openxr output/steamvr-openxr-test
```

The build verifies the cached SDK archive checksum and cross-compiles against
the pinned FEX rootfs. The runner verifies bundle hashes and requires this
user's active private null-headset session. It sets `XR_RUNTIME_JSON` only for
the sample, leaving the native ARM64 Monado runtime selection intact. Run as
the desktop user. The result does not cover OpenXR input or VR game launch;
the separate controller probes retain their own evidence.

Evidence: `output/steamvr-openxr-maintained-v1/` and
`output/steamvr-openxr-final-v2/`.

## ARM64 client error 101 and software presentation

The reported **Installation Corrupt (101)** dialog is reproducible even though
the x86-64 SteamVR files are present. A targeted syscall trace identifies the
native ARM64 Steam client checking `SteamVR/bin/linuxarm64`, which is absent
from the installed public SteamVR package. Valve's
[OpenVR loader](https://github.com/ValveSoftware/openvr/blob/master/src/openvr_api_public.cpp)
returns that error when its architecture-specific runtime directory is missing.
The translated components separately find `bin/linux64` successfully.

`tools/desktop-steam.sh` now adds `-vrdisable` when the installed runtime has an
x86-64 client library but no ARM64 client library. This skips the native Steam
client's incompatible in-process VR integration; Steam can still launch the
x86-64 SteamVR tools through FEX. The flag was verified in a temporary VM launch:
the 101 dialog disappears, while the actual SteamVR monitor still reports a
compositor connection error. Three launcher tests verify both architecture cases,
the absent-runtime case and preservation of `-applaunch` arguments. The final launcher was installed in the persistent VM. Its installed
SHA-256 matches the maintained source:
`b3716233636e43fd43a016cabf5ecdb6d5165c736a8e158a6fb13b3380977ad0`.

`patches/mesa/0002-x11-software-present.patch` implements opt-in
`MESA_VK_WSI_SW_PRESENT=1` for the software Vulkan X11 path without DRI3/MIT-SHM.
Upstream's fallback uploads directly to the window with XPutImage, which produces
no Present pixmap completion events. The patch uploads to ordinary X server
pixmaps and submits them through the existing XPresent queue. It reuses images
after actual idle events, retains presentation serials, damage regions and MSC
scheduling, and releases its pixmaps on destruction. It does not generate fake
notifications or change SteamVR's counters. The normal unselected path is retained.

The pinned Mesa build in `output/mesa/x11-present-v1/` passed with both patches,
using 1,672,404,992 bytes at peak and no OOM events. An independent X11 observer
sees zero pixmap completions with the original software path and advancing real
completions with the opt-in path. Twelve native/FEX runs pass immediate, mailbox,
FIFO, relaxed FIFO, incremental presentation and resize/recreation tests. Each
Vulkan sample exits normally. A separate translated Xlib Vulkan sample and x86-64
observer using SteamVR's own `libXpresent.so.1` also pass. Its log confirms both
Khronos instance and device validation layers loaded, with no validation errors.

The maintained observer can be built inside the development VM with:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror tests/xpresent-observer.c \
  -lxcb -lxcb-present -o xpresent-observer
```

Run it beside the test application on the same **private** X server; it requires
exactly one visible window. `--resize` also requires ten real completions after
the server confirms the requested resize. The observer does not render frames.

Both native and Windows/Proton OpenXR regression runs also pass with the new
driver: 599 submitted projection frames per run, 600 pose samples and four
captures verifying the initial, left-pressed, right-pressed and released eye
colors. Khronos instance/device validation layers are loaded with no validation
errors. The Windows loader log explicitly identifies the new native Mesa library.
The first isolated Windows executable copy lacked its three C++ runtime DLLs;
using the already-installed complete sample bundle resolves that harness error.
Evidence: `output/xpresent-followup-v1/` and `output/xpresent-windows-v2/`.

A bounded follow-up requested one real X-server NotifyMSC event after selecting
Present input. The server delivered that timing event, but the SteamVR probe
still reported PID 0, zero presentations and zero compositor submissions after
240 accepted eye submissions. The trace contains no `vkQueuePresentKHR` call.
The first attempt timed out during calibration; the second completed startup
and failed the frame test. This diagnostic is not part of the maintained
launcher. Logs and checksums are in `output/steamvr-bootstrap-v2/`.

An anonymous Valve SteamCMD query on September 10 identifies beta build
25216780, compared with the installed public build 23791826. Valve's
[current release notes](https://steamcommunity.com/app/250820/announcements/?l=english)
list beta 2.17.9 and earlier Linux headset fixes. The metadata query alone does
not establish a native ARM64 compositor or a fix for the virtual display stall.
The isolated query log is preserved alongside the follow-up diagnostics.

A separate beta 2.17.9 run verifies 3,739 files against the three accessible
Linux/shared/HTC depot manifests and uses private runtime, configuration and
log paths. The original installation is mounted read-only. This run also
**fails**, but differently: connection and room setup succeed, then the first
eye submission returns error 106 (`SharedTexturesNotSupported`). The
compositor exits before stack inspection; the terminating signal was not
captured. No queue presentation was observed. Missing `libnss3` and Steam
runtime relaunch warnings also remain in this isolated environment, so this
does not establish a complete supported beta runtime environment.

The accessible Linux manifest contains `bin/linux64`, with no `linuxarm64`
compositor. Anonymous access to the separate Linux DLC is denied; its contents
are unknown. Logs, verified-file records and the exact beta compositor are
retained in `output/steamvr-beta-v1/`. The beta run does not replace the public
build's distinct result below or establish that either release works in VR.

A second bounded beta run uses the actual QEMU desktop X server with its
unmodified XPresent library, instead of the private Xvfb server and tracing
library. A read-only XRandR query measures the active 1600×900 mode at
74.999005 Hz. Seven already installed x86-64 NSS/NSPR libraries from Sniper are
made available through a private dependency directory; the original SteamVR
installation and OpenVR registry remain read-only in the test.

Calibration completes after 782.76 seconds, connection and room setup pass,
and the first eye submission again returns 106. This time the launcher records
the original compositor process exiting with `-11` (SIGSEGV). Immediately after
startup, its logged HMD frequency changes from approximately 30 Hz to 0 Hz;
the earlier Xvfb run produced an enormous value whose float bytes decode to
`mp/s`. Those observations do not identify the faulting instruction or establish
that refresh-rate handling caused the crash. A valid X server mode did not
resolve the failure. The subsequent automatic relaunch also reports a missing
Qt `xcb` platform plugin, so a complete supported runtime environment remains
unproven. No SteamVR frame acceptance passed. The unit has stopped; evidence
and hashes are in `output/steamvr-beta-desktop-v1/`.

Before the corrected timing bootstrap, the full SteamVR run with this patched driver **failed**: 120 valid poses
and 240 accepted eye submissions, but PID 0, zero frame submissions and zero
presentations in compositor statistics. The independent presentation fix alone
did not establish a working SteamVR dashboard or game. The evidence and hashes
are in `output/xpresent-validation-v1/`. This software path also does not implement
the separate KGSL synchronization or physical headset display port.

## Original graphics blocker

Valve's null driver recognizes the simulated headset. With
`steamvr.allowFallbackMirrorWindowLinux=true`, the compositor creates windowed
headset and mirror swapchains and initializes Vulkan. The setting is declared
in [Valve's OpenVR header](https://github.com/ValveSoftware/openvr/blob/master/headers/openvr_capi.h).
The initial `VRInitError_Compositor_CannotDRMLeaseDisplay` is resolved by this
setting; it must be applied while SteamVR processes are stopped so their cached
configuration does not overwrite it.

The next failure is an exportable semaphore created by `vkCreateSemaphore`.
SteamVR reports `UNKNOWN_ERROR` at `vulkanrenderer.cpp:2565`. A separate native
ARM64 probe, without FEX or Steam, reproduced:

```text
GPU: llvmpipe (LLVM 22.1.8, 128 bits)
binary opaque-fd: features=0x0 compatible=0x0
create binary export=0: VkResult=0
create binary export=1: VkResult=-1000072003
timeline opaque-fd: features=0x0 compatible=0x0
create timeline export=0: VkResult=0
create timeline export=1: VkResult=-1000072003
```

`-1000072003` is `VK_ERROR_INVALID_EXTERNAL_HANDLE`. The Khronos validation
layer reports `VUID-VkExportSemaphoreCreateInfo-handleTypes-01124`: OPAQUE_FD
export is unsupported. Ordinary local semaphores succeed. This isolates the
failure to graphics interoperability rather than authentication or CPU
translation.

[Lavapipe's synchronization implementation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.8/src/gallium/frontends/lavapipe/lvp_pipe_sync.c)
has sync-file import/export callbacks, but no opaque-FD callbacks. SYNC_FD and
OPAQUE_FD are different Vulkan handle types and cannot be substituted. Simply
enabling another extension name does not implement the missing synchronization.

The supported capability check is:

```sh
just check-steamvr
```

It inspects every Vulkan device in the running VM. Exit 2 means no device
advertises the required binary and timeline semaphore import/export support;
exit 1 is an inspection error. Exit 0 only means that a candidate advertises
the capability. Actual export/import across processes, the compositor,
dashboard and applications must still pass. The current result is saved in
`output/desktop/steamvr-interop.txt`.

## Implemented DRM synchronization backend, 2026-09-10

QEMU's `virtio_gpu` render node supports DRM binary and timeline synchronization
objects. A separate unprivileged process test exported/imported an opaque file
descriptor and exchanged timeline values 5 and 9. This provided a real kernel
primitive for Lavapipe to use.

`patches/mesa/0001-lavapipe-drm-sync.patch` adds opt-in `LVP_DRM_SYNC` support
to [Mesa 26.1.8](https://docs.mesa3d.org/relnotes/26.1.8.html), matching the VM.
It uses Mesa's existing DRM synchronization provider, replaces the emulated
timeline type when enabled, and signals DRM objects only after the llvmpipe
rendering fence completes. Capability queries use the runtime's actual import
and export callbacks. Ordinary process-local binary synchronization retains
its existing implementation. An explicitly selected unusable node fails device
initialization; leaving the option unset retains the default backend.

```sh
just build-mesa-tools
just build-mesa
```

The build runs offline after verifying Mesa's published archive checksum. It
uses two CPUs, at most 4 GiB RAM/swap, one linker thread and a 30-minute compile
timeout. Source fingerprints are checked before/after compilation. Results
include the driver, ICD manifest, capability probe, behavior test, package
inventory, hashes and resource counters. It does not install the driver.

The clean build in `output/mesa/verified/` passed and produced the exact same
15,041,368-byte driver tested in QEMU. Peak memory was 1,435,615,232 bytes;
no OOM events occurred. Driver SHA-256:

```text
e8cdcfc2fc7b1f743cd7d4c0a7756e99b9ee4a9952afce18b2f7971f55c96737
```

The current VM has the driver under `/opt/armada-vr/mesa`, separate from its
system driver. Tests select it per process with:

```sh
VK_DRIVER_FILES=/opt/armada-vr/mesa/share/vulkan/icd.d/lvp_icd.aarch64.json
LVP_DRM_SYNC=/dev/dri/renderD128
```

`tests/vulkan-external-sync.c` starts independent Vulkan instances using
fork/exec and passes opaque FDs over a Unix socket. Both binary and timeline
tests verify a 4 MiB shared buffer written by queue commands in each process.
The timeline reaches 9. Both pass with the Khronos validation layer confirmed
loaded and no validation errors. The system driver fails the same test with
`VK_ERROR_INVALID_EXTERNAL_HANDLE`. These are scoped behavior tests, not CTS
conformance or proof of every fence/image-sharing operation.

`tests/vulkan-external-image.c`, included in native probe bundles as
`vulkan-external-image`, separately checks actual image sharing. Sixteen cases
cover RGBA/BGRA UNORM, optimal/linear tiling, render/sample/input usage with and
without storage usage, and red/blue content. The exporter finishes its queue
work and releases ownership to `VK_QUEUE_FAMILY_EXTERNAL`; a fresh exec process
imports its opaque FD on the same device UUID, acquires ownership, copies the
image to a buffer and checks all 4,096 pixels. Every case also checks the parent
FD count returns to its initial value. Run it as the ordinary lab user with the
selected native Vulkan ICD. Missing format/handle support or a wrong pixel is
a failure, not a skipped case.

The original fixture passes all 16 cases / 65,536 pixels on the Quest QEMU
kernel and pinned Mesa 26.1.8, with no leaked parent FDs. It serializes export
and import through queue completion and process lifetime; it does not test
concurrent producer/consumer synchronization, SteamVR's image-manager IPC,
compositor rendering, display scanout or physical GPU behavior.

The same test was also cross-compiled against the VM's existing x86-64 rootfs
and passed both round trips through Valve FEX with native Vulkan thunks. Its
ELF architecture and source/binary hashes were checked separately. That rules
out a blanket FEX failure for these semaphore/FD/memory operations; it does not
prove SteamVR's distinct IPC protocol or every translated graphics call.

The existing Monado/Khronos stereo rendering test also passes with this backend:
44,606 changed pixels and 1,033 colors, with the captured stereo scene visually
inspected. Evidence is in `output/mesa/verified/render/`.

SteamVR's unmodified compositor mapped this exact native driver through Valve
FEX. Its full 20-sample benchmark completed in 378.57 seconds, reported 1 MP/sec,
and stored its own calibration. It then created both distortion meshes, the
system layer and frame manager, started its render thread and logged
`Startup Complete (379.088348 seconds)`. The earlier semaphore creation failure
did not recur. The status UI still displayed error 307, and monitor/room-setup
IPC connections disconnected after compositor startup. Resolving the session
and dashboard remains required; a running compositor thread is not a rendered
VR dashboard. The diagnostic session is capped at 3 GiB and ten minutes.

A clean restart completed compositor startup again in 394.42 seconds; it still
ran calibration and the existing monitor retained error 307. A fresh x86-64
background client, started after initialization, successfully called Valve's
`VR_InitInternal2` and obtained `IVRCompositor_029`, both with
`VRInitError_None`. Its own log confirms successful IPC responses from both
`vrserver` and `vrcompositor`. This narrows the next investigation to startup/session
orchestration and actual frame/overlay calls; it does not demonstrate dashboard
rendering. The diagnostic session then stopped at its ten-minute limit. The
background probe used the installed Valve API library and
[documented public exports](https://github.com/ValveSoftware/openvr/blob/master/src/openvr_api_public.cpp),
without changing Valve binaries or its recorded calibration.

This implementation is a VM development backend. The Quest's vendor display
driver does not enable `DRIVER_SYNCOBJ`, despite its kernel containing the core
ioctls. KGSL integration and hardware presentation remain separate port work.

## Earlier translation and calibration experiments

The compositor's default launch escaped Sniper through Valve's capability
launcher and used the OS FEX interpreter with x86 Lavapipe. An isolated test
instead used Valve's bundled FEX and ARM64 Lavapipe through its Vulkan thunks.
The Arch rootfs needs explicit `/usr/lib/libvulkan.so*` overlay paths; the
bundled database expands the non-Debian 64-bit prefix to `lib64`.

The native-thunk test created both swapchains and both eye distortion meshes.
For diagnosis only, two calibration limits were changed from 20 samples to
one in a hash-verified copy of `vrcompositor`. That sample took 104.09 seconds,
reported 0 MP/sec and let startup reach the semaphore failure. This was not a
valid performance result. The original Valve executable has been restored
and its SHA-256 verified:

```text
5df8de42b302e38c1b1ce9feb36e7f31c3d31d6c78e6be41090529db6bcbfc39
```

No binary patch is included in the build. Diagnostic sessions had explicit
timeouts and memory limits. The VM remains capped at 6 GiB RAM and two CPUs.

## Earlier frame, controller and translation tests, 2026-09-10

`tools/steamvr-session.py` uses `vrserver -keepalive`, waits for a **new**
compositor startup message and then starts the monitor. Its startup and session
deadlines are finite; signal handling stops its own process groups. The tests
reject stale completion messages and premature process exit. Diagnostic runs
also use a 3 GiB systemd memory limit and a finite unit lifetime.

The probe and simulated controller driver build against Valve's checksum-pinned
OpenVR headers through `tools/build-steamvr-probe.sh`. The frame test obtains
120 valid HMD poses and successfully submits both eyes 120 times. Before the
timing correction, `GetCumulativeStats` reported PID 0, zero presents and zero
frame submissions. The test correctly failed. The compositor render thread was observed waiting
on its X11 socket, with software-rendering workers idle. This narrows the
investigation; it does not establish which translated X11 call is responsible.
FEX's guest debugger socket did not answer GDB's protocol negotiation.

The `armada_virtual` driver supplies two simulated controllers, left/right
roles, bounded poses/buttons and a haptic event counter. Discovery, changing
poses and haptic request delivery passed initially. Before the timing correction,
the modern OpenVR action probe reported inactive actions and `IsInputAvailable=0`;
trigger/grip acceptance failed. Those actions pass in the corrected runs above.
Neither this driver nor the null HMD accesses physical headset devices.

An independent translated `vkcube` test exposed a missing FEX thunk lookup for
`vkGetDeviceProcAddr(device, "vkGetDeviceProcAddr")`. The scoped guest library
in `src/vulkan-procaddr.c` resolves that entry and forwards other procedures.
It makes both FIFO and immediate 60-frame cube runs pass. The upstream-source
backport in `patches/fex/` passes a dry application check but has not been built.
The library is selected through the launcher's `--resolver` argument and
`FEX_ENV`; it is never preloaded into native ARM64 applications. It does not
resolve the remaining SteamVR presentation stall.

Evidence: `output/steamvr-session/diagnostics-v11/` and the VM session logs.
Windows OpenXR now has a separate working Vulkan 1/2 sample path through Monado;
see [its launcher and tests](windows-openxr.md). That result does not establish
SteamVR dashboard or OpenVR game compatibility.

The [FEX project's VR page](https://wiki.fex-emu.com/index.php/VR), checked on
2026-09-10, also reports SteamVR as nonfunctional in its documented setup.
That is supporting context, not the diagnosis of this VM's specific X11 wait.
Its advice about x86 Linux OpenXR runtimes is distinct from the tested ARM64
Wine Unix bridge used by this project's Windows sample.

## Further presentation isolation

On the Quest-derived QEMU kernel, native GDB traces the render thread into the
guest's `XNextEvent`/`XGetEventData` loop waiting for a matching Present completion.
A separate XPresent test gets non-advancing zero UST/MSC values from this VM's
unaccelerated Xorg display. On Xvfb, the same test receives ten advancing
notifications. Running SteamVR on Xvfb still fails the real frame test, so the
Xorg timing issue alone does not explain or resolve the stall.

Two private diagnostic libXpresent builds, one tracing calls and one disabling
extension detection, also fail presentation. Neither is installed by the
maintained launcher. Providing the installed Sniper libraries and starting the
monitor before the probe allows a web-helper compositor connection but still
fails the frame test. Successful `Submit` calls remain insufficient evidence.
These experiments do not modify Valve's binaries or count as rendering fixes.

## Next development step

Actual OpenVR/OpenXR presentation, dashboard rendering and isolated controller
acceptance now have passing evidence above. Remaining virtual work includes
stock Library navigation, the nonfinite smooth-scroll issue, Steam library game
launch, a longer soak, and a fresh image that reproduces the complete installed
SteamVR stack. Windows focus transitions now pass for both hands.
A native Linux GPU environment can also run the virtual headset tests without
accessing a Quest or Pico.

The software-rendered VM now has the tested synchronization backend above.
Venus is not a confirmed shortcut: [Mesa 26.1.8's Venus implementation](https://gitlab.freedesktop.org/mesa/mesa/-/blob/mesa-26.1.8/src/virtio/vulkan/vn_physical_device.c)
advertises SYNC_FD for external binary semaphores and no external timeline
semaphore handles. Check the required handle types before building another VM.

Hardware flashing stays outside
the workflow until the VR software milestones and device recovery prerequisites
are complete.
