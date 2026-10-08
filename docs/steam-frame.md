# Steam Frame software reuse in Armada VR

For the October 8 update and current implementation priorities, see
[device installation requirements](device-installation.md). The findings below
retain their original investigation dates.

## October 8 firmware comparison

The existing official recovery image is `20260922.5153644-0.3.0`; its native
SteamVR binaries are already the inputs used by the ARM64 tests. Avoid downloading
another copy when researching the public firmware.

Valve's [stable VR metadata](https://steamdeck-atomupd.steamos.cloud/meta/holo/steamos/aarch64/vr/stable.json)
now names `20260922.6101926`, also version `0.3.0`. The recovery image's updater
configuration joins its `update_path` to `https://steamdeck-images.steamos.cloud/`,
not the metadata server. The resulting 2,275,426-byte RAUC bundle contains a
signed manifest and a casync index for a 10 GiB root filesystem. Offline CMS
verification succeeds against the certificate in the recovery image's
`/etc/rauc/trusted_keys/`. That establishes consistency with this trust anchor;
the recovery image itself was acquired over official HTTPS and has not received
independent manufacturer-signature authentication. No update installer ran.

The image actually packages systemd `257.7-2.2`. Its
[upstream requirements](https://github.com/systemd/systemd/blob/v257/README)
have a minimum kernel version of 3.15 and recommend 5.4. This makes Frame userspace
a candidate to test with Pico's 4.19 vendor source; the current Fedora/systemd 259
combination has an [upstream minimum of 5.4](https://github.com/systemd/systemd/blob/v259/README).
Selected systemd features may still need backports, and
neither the nominal version floor nor the Frame image proves Pro Eye boot or
driver compatibility. Preserve the Pico board, eye calibration and firmware
requirements.

[SteamOS-ARM-Port](https://github.com/hashtagbasit/SteamOS-ARM-Port/blob/aab5fea4bd153e54dc76d4ae17c4ff871a66b506/docs/HOW-IT-WORKS.md)
combines Frame userspace with device-specific ROCKNIX/kernel/display support.
That separation is applicable here. Its handheld ABL, display setup and controller
integration are not Quest or Pro Eye boot and 6DoF implementations. Maintain
matching Mesa Vulkan and OpenGL components when testing Turnip/Zink; replacing
one library can break applications using the other API.

## Earlier runtime investigation

Since that investigation, original Lepton v3.0.5 source and official Frame
repair images became public. October 7 offline inspection obtained native
ARM64 SteamVR client/server/compositor binaries;
[ABI, native-server and virtual-device results](steamvr-arm64.md)
are documented separately. [Rootless Podman/FUSE mount acceptance](android-containers.md#podman-writable-mount-acceptance)
now passes on the unchanged Quest test kernel. An October 8 authenticated Steam
installation also supplies the [Lepton payload; actual Android boot, Binder,
persistence, software APK rendering and networking pass](android-containers.md#acquired-lepton-and-actual-android-boot).
APK input also passes. Application lifecycle, Android XR and native SteamVR
backend rendering remain unverified. Docker is healthy. The
September results below are historical, including their access and Docker limits.

The September 14, 2026 investigation finds useful public runtime changes,
controller contracts and development tools, but no newly obtainable complete
Frame OS or native Valve SteamVR bundle. Armada VR already has native ARM64
Steam, ARM64 Proton, Monado and a Quest-specific Turnip/kernel path. The first
new implementation is a reproducible diskless Android-container kernel test;
it proves BinderFS isolation and identifies rootless OverlayFS as an unresolved
Lepton integration requirement.

Valve's current hardware page opens reservations: queue allocation is scheduled
for September 17, with purchase emails beginning September 18. Its linked
announcement is titled “Steam Frame is here!”. This establishes today's public
launch event, without establishing retail delivery or public availability of
every device package. [1]

## Component availability and decisions

| Component | Verified public or local state | Implementation decision |
|---|---|---|
| Steam ARM64 client | Already pinned locally to publicbeta version `1788652215` | Keep the native client; do not wrap it in x86 FEX |
| Steam Linux Runtime 4 ARM64 | Latest public stable remains `4.0.20260805.254769`, identical to the local archive pin | Keep the verified runtime |
| Proton ARM64 | Official 11.0-2 source and ARM64 build instructions; no GitHub release binary assets | Preserve verified CachyOS Proton while preparing a separate official-build comparison |
| FEX | FEX-2609 released September 8; the project has multiple distinct FEX sources | Identify the actual executable and build options before replacing or patching it |
| SteamVR | Public 2.17 release; no obtainable complete native ARM64 client/server/compositor found in this pass | Keep native Monado development and the x86 SteamVR compatibility baseline |
| Lepton | Officially documented; a community depot inspection describes a newer launcher/image pair | Resolve original package access and host runtime dependencies before integration |
| Frame developer tools | Public, pinned GitLab source | Adapt individual diagnostics after inspecting device/path assumptions |
| Frame hardware drivers | Restricted OS repositories; public upstream Mesa and Linux work | Reuse applicable source changes while keeping Quest board support |

Availability is scoped to the inspected public endpoints. A public SDK loader,
store entry or package name does not establish that the corresponding complete
runtime was downloaded or can be redistributed.

## Runtime architecture

Valve documents three application paths: Windows games through Proton, x86 code
through FEX, and Android games through Lepton. FEX can forward graphics calls
to native host libraries. Frame itself runs an ARM64 Snapdragon 8 Gen 3 on
SteamOS. These facts support keeping native host graphics and XR services, but
do not specify that every internal compositor component is open or directly
portable. [2]

Armada's native Linux Monado path and its ARM64 Proton/OpenXR test path already
follow that direction. Its desktop SteamVR path still executes x86 binaries
through Valve's FEX tool. That is a compatibility baseline. The Wine-side x86
OpenXR bridge exists because that particular SteamVR client library is x86;
it is not a requirement to translate all future headset rendering.

There are three independent compatibility boundaries to check when new files
become available:

1. CPU architecture: an ARM64 loader cannot load an x86 ELF library.
2. Userspace ABI: Android Bionic and Linux glibc ARM64 libraries are different
   builds even when their ELF machine field is the same.
3. XR protocol and lifecycle: a loader or client library still needs its matching
   service/compositor, device driver, runtime paths and graphics-sharing support.

No newly discovered package removes the outstanding fresh-backend Windows
rendering or both-hand dashboard acceptance requirements.

## Proton and Steam Runtime

Valve's latest inspected Proton release is **11.0-2**, published August 21,
at commit `db9e6ffbf24a95b104fb699dd62532c70a2f9a51`. Its source pins Wine
`dc26e61847081a1b5cb0733dc30feba6ee575482` and FEX
`1cc4b93e7a71c883ec021b71359f136394dc1f3c`. The GitHub release has no attached
prebuilt distribution; its automatic source archives should not be mistaken for
an installable ARM64 Proton tool. [3]

The inspected `proton_11.0` branch is
`5b89db940e0ebe3a137a6009a3589232fe084c09`. Its README requires an ARM64 build
machine and `--target-arch=arm64`; it explicitly says the resulting tool cannot
be used with an x86 Steam client under FEX. The ARM64 tool manifest requires
Steam runtime app `4185400` and enables sessions. [4]

Valve's runtime distribution reports exactly the version Armada already pins.
`SteamLinuxRuntime_4-arm64.tar.xz` SHA-256 remains
`caa4b3bc3aad1cac43d94dbd802a963c13ed63ea1c414f04669ae402af505adf`.
No archive update is warranted merely because the hardware launch occurred. [5]

The current July CachyOS ARM64 Proton build remains the known working input.
An official-build comparison should use a new prefix and output directory,
record its Wine/FEX/runtime identities, test native and Windows OpenXR, and
preserve the existing resolver workaround until its triggering behavior is
retested. Replacing a version string without a compiled and exercised payload
would not complete that comparison.

## FEX changes and actual applicability

FEX-2609 resolves to commit `395b132f346b1a45def246d10c52245edba1ef02`.
Its new disk cache is opt-in. Upstream documents missing size limits and
stale-entry cleanup, plus invalidation caveats. Keep it disabled by default;
any later comparison needs a bounded disposable cache and real workload
measurements. VM functional results cannot establish headset performance. [6]

Two upstream changes are relevant:

- `9618b5adef54fb5c585d6aceb1f6a070d2faccf6` makes Steam FEX options tri-state,
  preserving unspecified defaults while honoring explicit zero and one values.
  This matters for per-game TSO and graphics-thunk choices. [7]
- `71afe476751deac24adabd1adb575fd2337b6e0a` adds pressure-vessel graphics
  locations to thunk-library prefix discovery when Steam support is built in.
  The paths include `/run/gfx/main/usr/lib/<arch>` and the pressure-vessel
  override directory for non-multiarch guests. [8]

The local audit found an important distinction. Armada's package source is
pinned to FEX revision `e869aa644a16e4332cdc15c1ea0b4d13d482385d` and includes
ROCKNIX-derived patches. Its RPM configuration does not enable upstream
`BUILD_STEAM_SUPPORT`, which defaults off in that source. The lookup patch
applies textually but its guarded code would be inactive.

The three newer FEX source snapshots previously examined by this project already
contain the lookup change. Source inspection does not identify the installed
Valve binary revision; that still needs a fresh runtime inventory. The adjacent
package recipe also does not independently attest the pinned OCI binary.

Separately, `tools/steamvr-session.py` selects Steam's own
`steamapps/common/FEX-Emu/usr/bin/FEX` and supplies absolute Vulkan thunk overlay
paths. A patch to the Armada RPM would not alter that process. Proton also pins
its own translation integration. These must be treated as separate build and
runtime identities. The pressure-vessel patch was therefore not added as an
unused local fix. Full rebuilt-FEX and rendering tests remain pending.

## Native SteamVR and SteamVR 2.17

Valve published SteamVR 2.17 on September 10. The release includes dashboard
and cursor changes, Linux improvements and 32-bit OpenXR support. Its laser
hand-switch fix is listed under Steam Link hand tracking, so it cannot be
assumed to fix Armada's custom virtual-controller dashboard issue. A new
backend still needs both-hand selection, scrolling, menu and focus tests. [9]

The public SteamVR tracker at
`e8b555a466b67d25de61360ec2c5c109c32bb720` provides Deckard input resources.
Its complete tracked tree contains no `linuxarm64` or `androidarm64` runtime
directory. The Deckard profile is an HMD head-pointer/button profile, not a
two-hand controller implementation. It must not replace the current controller
tests. This is evidence about the tracker, not proof that Valve has no private
ARM64 build. [10]

The project's September 11 search already covered SDK loaders, Holo preview
packages and denied depots. This pass followed the new official Frame source
links. The Frame public GitLab group exposes developer tools, while the OS
package endpoints described below remain restricted. No complete native
`vrclient`/`vrserver`/`vrcompositor` set was obtained. The earlier access-denied
depot download was not repeated without a new route.

## Lepton and the implemented kernel test

Valve confirms that Lepton runs Android applications in a Linux container. Its
developer documentation describes launching Lepton Development and connecting
with ADB for ordinary Android debugging. That does not imply Lepton supplies
Quest firmware or makes a glibc OpenXR runtime usable in Android. [2][11]

An original community inspection now describes Lepton tool **2.8.14**, dated
September 14, and image **2.8.11**, dated September 9. It reports a Waydroid-based
Android 11 guest, rootless Podman, BinderFS and host-mounted graphics/XR
components. Its OpenXR manifest points at an Android ARM64 SteamVR client in
the host runtime. The author explicitly lacked the host runtime and OS overlay;
some transport details remain inference. No original launcher source archive
was obtained here, so those implementation details remain reported findings,
not locally reproduced Lepton behavior. [12]

The new `tools/test-android-container.py` performs actual kernel probes with a
small static ARM64 fixture. It verifies the retained kernel manifest, boots
diskless QEMU without network or device passthrough, and records source hashes,
compiler version, commands and serial output. Docker is unnecessary.

On the existing `qemu-abi-v10` kernel, it verifies:

| Measured interface | Result |
|---|---|
| Unprivileged guest UID 1000 creating user/mount/PID/IPC/UTS/network namespaces | Pass |
| Two BinderFS mounts with three distinct device contexts each | Pass |
| Binder protocol 8 and independent context-manager registration | Pass |
| Duplicate-manager rejection and cross-instance device removal isolation | Pass |
| Shared memfd writes across a fork and write/size seals | Pass |
| Seccomp denial and allowed-syscall behavior | Pass |
| Native rootless OverlayFS mount | **Unsupported: `EPERM`** |
| Private mount cleanup and guest shutdown | Pass |

The source inspection agrees with the measured limitation: BinderFS has
user-namespace mount support, while this vendor OverlayFS implementation lacks
it. The kernel's existing `CONFIG_OVERLAY_FS=y` was therefore insufficient.
The Binder results follow the kernel's documented independent-instance model. [13]

Podman documents `fuse-overlayfs` for older kernels. Selecting that helper is
not yet a verified fix for Lepton: its image-storage mount and writable Android
data mount may be separate operations. The next step needs the actual launcher
and a test of its complete rootless mount lifecycle. A reviewed kernel backport
is another option. Neither fallback has been silently enabled or declared
working. [14]

The successful evidence is `output/android-container-v4/result.json`. Its
`lepton_runtime_verified`, `hardware_verified` and `flash_image` values remain
false. This test does not boot Android, run an APK, exchange Android service
transactions, exercise a graphics driver or validate multiple complete
containers. [Usage and exact limits](android-containers.md) are documented
separately. Failed fixture setup runs are retained alongside the successful one.

## Graphics, kernel and firmware reuse

Igalia's engineering report identifies Frame's Adreno 750 and Turnip work,
including improvements that also benefit older Adreno generations. This
reinforces Armada's existing use of Mesa/Turnip. It does not imply the GPUs,
kernel display paths or firmware are interchangeable. [15]

Armada currently pins Mesa 26.1.8 and carries KGSL synchronization and explicit
DRM-display integration for the Quest's recorded Adreno740v3 variant. The
retained Meta 5.10.246 source, Eureka/Anorak device trees, panel definitions,
firmware inventories, thermal work and Qualcomm service startup remain the
board-specific foundation. The native Monado build is also retained.

Valve's debugging documentation names an Android Mesa package,
`deckard-mesa-android-aarch64-debug`. Its example development version is not a
current package manifest. More importantly, that package is an Android ABI
build; Armada's glibc Turnip library cannot simply be mounted into the same
slot. [11]

Existing Armada/ROCKNIX reuse still covers the Fedora/package workflow, FEX
packaging and selected upstream patches. The headset port continues to need
its own Android boot/vendor_boot assembly, kernel modules, root handoff and
recovery checks. Frame's launch changes none of those Quest-specific gates.
The older handheld `/KERNEL` layout and updater do not become a Quest installer.

## Published developer tools and remaining restricted sources

Valve's public `frame-public/frame-developer-tools` repository is pinned to
`16d1942eede77e6cec5a7c40649a53b82cef472a`, dated September 2. It contains
Perfetto, GPUVis, GfxReconstruct and Gamescope helpers. Its README describes
developer tooling and includes old device/path assumptions; individual scripts
need inspection before use. [16]

The included `deckard-pacman-src` maps Frame package origins to
`potato/mash/monorepo`, `potato/smash/holo` and `deckard/deckardos/holo`.
Anonymous checks of its Frame package endpoints returned HTTP 401, and the
inspected source project APIs returned 404. Public group enumeration showed
only the developer-tools project. These mappings are useful source-location
evidence, not downloadable OS artifacts. [17]

The exposed profiling configurations name MSM GPU counters/render stages and
DRM vblank/dma-fence tracepoints. An Armada adaptation should discover the
available KGSL/DRM events and userspace producers first. A software QEMU GPU
cannot supply Frame's hardware counters. Profiling should be bounded and
explicitly distinguish frame submission, display presentation and actual
headset timing. [18]

The inspected recovery index still contained Steam Deck images, not a Frame
recovery image. No large firmware image was downloaded. Public source mirrors
also did not yield a complete new Frame board-support package in this pass.

## Controllers, tracking and other community work

Valve publishes `frame_controller` as the OpenVR controller type, with Frame,
generic OpenXR and Oculus Touch binding fallback. Its Frame-specific OpenXR
extension is `XR_VALVE_frame_controller_interaction`, with interaction path
`/interaction_profiles/valve/frame_controller_valve`; the suffix matters after
the SteamVR 2.15.1 rename. Applications should enable it only when advertised.
The existing Quest/Vive-style trackpad and application-menu requirements remain
in scope. [19][20]

Valve also documents foveation and eye-gaze extensions. These describe runtime
capabilities; they do not provide eye-tracking hardware on Quest 3. Synthetic
gaze or controller events must not be reported as physical tracking. The
documented SDK ARM64 client libraries likewise remain distinct from a full
XR runtime. [20]

The community MIT `ovrplugin-openxr-shim` at
`574f41a0e43e84a3b2f03a12a0167856139f6c6e` is a candidate for legacy OVRPlugin
application compatibility. Its author reports one Quest 2 game working, but
its Frame notes assume Monado without supporting Valve evidence. Treat it as
an ABI implementation to evaluate against specific applications, not a general
APK solution or a headset driver. [21]

Arcturus's published DIPr repository is older IMU fallback research, not a
released Frame SLAM/controller stack. Its noncommercial share-alike license
also needs separate consideration before reuse. No public drop-in Frame
tracking engine or reusable Quest calibration was obtained. [22]

Valve-funded scheduler work is another research lead, but `sched_ext` requires
kernel support absent from the Quest 5.10 baseline. Installing a scheduler
binary cannot add that kernel subsystem. Preserve the existing thermal and
power work and prioritize boot/runtime correctness before a substantial
scheduler backport. [23]

## Next implementation sequence

1. Obtain a verifiable original Lepton launcher/image and its exact host-overlay
   requirements. Resolve native rootless OverlayFS or a tested FUSE path; then
   exercise Android boot/shutdown and a small openly licensed APK offline.
2. Check Linux-versus-Android graphics/runtime ABI and buffer/fence sharing.
   Preserve the verified native Monado and translated SteamVR comparisons.
3. Recover Docker health before new container-based builds. Its endpoint still
   times out after storage recovery; no restart, reset or prune was performed.
4. Finish the matching kernel/startup/full-root integration and the incomplete
   SteamVR/FEX export using new output paths and current manifests. The earlier
   truncated root-overlay image remains invalid and must not be booted.
5. Test SteamVR 2.17 on a fresh backend: both-hand pointer ownership, dashboard
   selection/scrolling, stock Library navigation, menus, app focus and Windows
   frame presentation. Prior synthetic passes remain useful evidence but do
   not satisfy this full interaction gate.
6. Compare official ARM64 Proton and the correct FEX build when their payloads
   can be rebuilt and exercised. Adopt individual applicable upstream driver
   changes, with hardware timing tests deferred until the headset is present.

Exact-device boot acceptance and authenticated recovery remain unresolved.
No device connection, installation, unlock or flash occurred. The kernel probe
and runtime research do not establish flash readiness.

## Sources

1. Valve. [Steam Frame hardware and reservation FAQ](https://store.steampowered.com/hardware/steamframe); [launch announcement](https://store.steampowered.com/news/group/45479024/view/692020124159311887). Observed September 14, 2026.
2. Valve. [Standalone compatibility architecture](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility). Observed September 14, 2026.
3. Valve. [Proton 11.0-2 release](https://github.com/ValveSoftware/Proton/releases/tag/proton-11.0-2) and [pinned source tree](https://github.com/ValveSoftware/Proton/tree/db9e6ffbf24a95b104fb699dd62532c70a2f9a51). August 21, 2026.
4. Valve. [ARM64 build instructions](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/README.md#arm64-builds); [ARM64 tool manifest](https://github.com/ValveSoftware/Proton/blob/5b89db940e0ebe3a137a6009a3589232fe084c09/toolmanifest_arm64.vdf).
5. Valve. [SteamRT4 version](https://repo.steampowered.com/steamrt4/images/latest-public-stable/VERSION.txt); [immutable checksum manifest](https://repo.steampowered.com/steamrt4/images/4.0.20260805.254769/SHA256SUMS).
6. FEX contributors. [FEX-2609 release](https://github.com/FEX-Emu/FEX/releases/tag/FEX-2609). September 8, 2026.
7. FEX contributors. [Steam configuration semantics](https://github.com/FEX-Emu/FEX/commit/9618b5adef54fb5c585d6aceb1f6a070d2faccf6).
8. FEX contributors. [Pressure-vessel thunk paths](https://github.com/FEX-Emu/FEX/commit/71afe476751deac24adabd1adb575fd2337b6e0a).
9. Valve. [Introducing SteamVR 2.17](https://store.steampowered.com/news/app/250820/view/679635225181946198). September 10, 2026.
10. SteamTracking. [Pinned SteamVR resource tree](https://github.com/SteamTracking/GameTracking-SteamVR/tree/e8b555a466b67d25de61360ec2c5c109c32bb720). September 9, 2026.
11. Valve. [Steam Frame debugging](https://partner.steamgames.com/doc/steamhardware/steamframe/debugging).
12. utzcoz. [Original Lepton depot inspection](https://utzcoz.github.io/2026/09/03/steam-frame-lepton-architecture.html). September 3, updated through September 14, 2026; reported and inferred evidence distinguished above.
13. Linux kernel contributors. [BinderFS 5.10 documentation](https://www.kernel.org/doc/html/v5.10/admin-guide/binderfs.html).
14. Podman contributors. [Rootless filesystem requirements](https://docs.podman.io/en/latest/markdown/podman.1.html#note-unsupported-file-systems-in-rootless-mode).
15. Igalia. [Engineering contributions to Valve's hardware](https://www.igalia.com/2025/11/helpingvalve.html). November 2025.
16. Valve. [Frame developer tools, pinned source](https://gitlab.steamos.cloud/frame-public/frame-developer-tools/-/tree/16d1942eede77e6cec5a7c40649a53b82cef472a). September 2, 2026.
17. Valve. [Frame package-source mapper](https://gitlab.steamos.cloud/frame-public/frame-developer-tools/-/blob/16d1942eede77e6cec5a7c40649a53b82cef472a/pacman/deckard-pacman-src).
18. Valve. [MSM Perfetto configuration](https://gitlab.steamos.cloud/frame-public/frame-developer-tools/-/blob/16d1942eede77e6cec5a7c40649a53b82cef472a/perfetto/configs/config_snippets/msm_gpu.j2); [GPU ftrace events](https://gitlab.steamos.cloud/frame-public/frame-developer-tools/-/blob/16d1942eede77e6cec5a7c40649a53b82cef472a/perfetto/configs/config_snippets/ftrace/gpu.j2).
19. Valve. [Steam Frame controllers](https://partner.steamgames.com/doc/steamhardware/steamframe/controllers).
20. Valve. [OpenXR integration for custom engines](https://partner.steamgames.com/doc/steamhardware/steamframe/engines/custom).
21. daniel-lynch. [OVRPlugin OpenXR shim, pinned source](https://github.com/daniel-lynch/ovrplugin-openxr-shim/tree/574f41a0e43e84a3b2f03a12a0167856139f6c6e). August 8, 2026.
22. Arcturus. [DIPr research source](https://github.com/arcturus-vision/dipr/tree/803719d4758b54b1e4f00dc27e35a63eb68b8f85). March 10, 2022.
23. Linux kernel contributors. [Extensible scheduler requirements](https://docs.kernel.org/scheduler/sched-ext.html).
