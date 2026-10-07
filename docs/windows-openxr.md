# Windows OpenXR on ARM64

The Windows x86-64 Khronos `hello_xr` sample renders stereo through ARM64
CachyOS Proton 11, Steam Runtime 4 and Monado in QEMU. Both Vulkan bindings
pass with the compatibility fixes below. This is a Windows OpenXR
sample result, not a Steam library launch, SteamVR dashboard or physical
headset result. Application-level controller actions and haptic API calls also
pass through Monado; physical haptic output remains untested.

The installed ARM64 Proton bridge cannot directly load this SteamVR build's
x86-64 runtime into its native process. `output/steamvr-runtime-architectures/report.json`
records actual ELF headers and hashes: SteamVR's `vrclient.so` is x86-64, while
Proton's `wineopenxr.so` and native OpenXR loader are AArch64. The SteamVR
installation has no `bin/linuxarm64`. The cached FEX rootfs contains x86-64 Wine,
but no Wine OpenXR bridge. A separate, entirely x86-64 Proton/FEX test path now
renders through SteamVR. This does not add an ARM64 SteamVR client library.

The separate x86-64 CachyOS package now passes a Windows CPU baseline through
FEX, using its `x86_64-unix/wine-preloader`, matching Wine executable and
`WINELOADERNOEXEC=1`, as used by its bundled Proton launcher. The Windows process
writes the expected `sum=500500` result itself. Evidence and the exact environment
are in `output/proton-x86-v1/cpu-v6-*`. Directly invoking `files/bin/wine` had
entered a failing 32-bit `start.exe` path; the earlier failed runs are retained.
The runtime is on a separate read-only VM disk, with a disposable
`compatdata-steamvr-x86` prefix. The Windows OpenXR bridge loads after copying
three matching vkd3d DLLs from this Proton package's default prefix. Initial
runtime symlinks caused the loader to resolve the Steam-managed beta client;
verified hardlinks correct that layout. Restarting the full backend then
restores both native and Windows OpenXR device selection.

Windows run v6 submits 109 frames and SteamVR records 94 actual presentations
for its Linux process; v7 submits 249 frames and records 216 presentations.
Both identify SteamVR 2.16.7 and exit normally. The harness must keep stdin open:
the sample treats EOF as a quit request. A normal newline ends the rendered
test. Captures show the sample's colored cubes, although its desktop mirror
obstructs most of the left-eye view. These runs establish sample rendering;
they do not establish full stereo-capture acceptance or game compatibility.

A fresh backend repeat on September 11 restores shared-image allocation. Run v10
records 227 OpenXR frames, 344 compositor presentations and normal exit; its
capture shows both eyes unobstructed. Run v11 initializes a new prefix using
`wineboot --init`, without copying the earlier prefix, then records 293 frames,
248 presentations and normal exit. This repeat does not establish the cause of
the earlier allocation failure after disk exhaustion.

Both hands now transfer Windows application focus through the dashboard. Runs
v12/v13 show the Windows sample changing from `FOCUSED` to `VISIBLE` when the
controller system button opens the dashboard, then returning to `FOCUSED` when
it closes. Each hand's application-menu button exits the sample normally;
these tests never send a console newline. Runs v14/v15 additionally measure
32 new haptic events at each virtual controller while its trigger is held with
application focus. The sample's binding-source enumeration says its vibration
action is bound to nothing, but its haptic calls reach the actual virtual driver;
that enumeration alone is not a delivery test.

Evidence is in `output/proton-x86-v1/openxr-v10-*` through `openxr-v15-*`.
Windows game compatibility, a complete reproducible SteamVR image, sustained
reliability and physical controller output remain unverified.

## Launch in the active virtual SteamVR session

`tools/run-steamvr-windows.py` discovers the current compositor's FEX interpreter,
private registry and graphics environment. It checks the OpenXR runtime path,
uses matching x86-64 Wine/preloader components and preserves existing prefix
files when they differ from the selected Proton package. It does not read any
historical test environment file or hardcode a backend unit or display number.

`tools/build-steamvr-probe.sh` includes this launcher and an x86-64 build of
`src/openxr-procaddr.c` in the virtual SteamVR bundle. The launcher defaults to
that adjacent resolver. Run the built copy as the desktop user with an active
virtual SteamVR backend:

```sh
python3 -B /path/to/steamvr-bundle/run-steamvr-windows.py \
  --proton /path/to/x86-proton/files \
  --launcher /path/to/windows/openxr-launcher.exe \
  --prefix /path/to/new-dedicated-prefix --initialize \
  /path/to/windows/hello_xr.exe -g Vulkan
```

Omit `--initialize` when reusing that prefix. Initialization refuses an existing
prefix and waits for the matching wineserver to finish after `wineboot`; the
first test exposed a race where `wineboot` returned before registry persistence.
The failed prefix and logs remain in `output/steamvr-windows-launcher-v1/`.
The corrected launcher passes from a minimal environment with another new
prefix: 489 OpenXR frames, 787 actual compositor presentations, focus transfer,
68 virtual haptic deliveries matching the sample's calls, and normal exit from
the right controller's menu. An existing-prefix rejection leaves its registry
hash unchanged. Evidence is in `output/steamvr-windows-launcher-v2/` and
`output/steamvr-public-v1/backend-v16-report.json`. All 80 host checks pass.

Bundle v10 now includes that launcher and the x86 resolver. All 17 bundle-file
hashes and ten source hashes match. On fresh backend v17, another newly
initialized prefix passes with 404 OpenXR submissions, 710 compositor
presentations, focus transfer, 62 matching virtual haptic deliveries and normal
menu-driven exit. Captures show unobstructed stereo and the stock Now Playing
panel over the scene. Evidence is in `output/steamvr-windows-launcher-v5/`.
The earlier v3 attempt rendered but failed its dashboard-close stimulus and
exited without a result report; its evidence is retained in
`output/steamvr-backend-v17/`. After stopping the backend and verifying all
3,544 runtime files, bundle v10 was installed at `/opt/armada-vr/openvr`.
The previous bundle remains at `/var/tmp/steamvr-before-install-v7`;
`output/steamvr-bundle-v10/install-v7.json` records the installed hashes.
Both installed command entry points pass `--help`; runtime testing above used
the identical bundle in its staging directory.

The ARM64 resolver cannot be substituted in this x86 process. When running
directly from the source tree, pass `--compat` with the built x86 resolver path.
A fresh complete SteamVR image remains required. Keep stdin open for `hello_xr`,
or its console thread requests shutdown immediately.

## Relationship to Steam Frame

The [ARM64 runtime search](steamvr-arm64.md) records the official and unofficial
sources checked, their access limits, and the missing runtime components.

Valve's [Steam Frame compatibility documentation](https://partner.steamgames.com/doc/steamhardware/steamframe/compatibility?l=english),
checked on 2026-09-11, describes Windows x86 games running through Proton and FEX,
with OpenGL and Vulkan calls forwarded to native host libraries. It also supports
native ARM64 and Android applications. That documents the game compatibility
strategy; it does not establish the architecture of Frame's compositor.

The current VM uses the available x86-64 desktop SteamVR runtime through FEX,
including its compositor. This is a compatibility test path, not a claim about
Frame's internal implementation or the final headset architecture. Its Vulkan
calls reach native ARM64 Lavapipe, which renders on the CPU. VM timings therefore
do not establish headset performance. Native ARM64 Proton with Monado is a
separate tested path. Native tracking, composition and GPU execution remain the
headset implementation target where the available runtime supports them.

## Guest CPU compatibility

On this Apple Silicon host, the Fedora 7.1 guest exposed SME while ordinary
SVE was absent. Native ARM64 Wine faulted in `ntdll!RtlUserThreadStart` before
running the Windows test; GDB also failed to read the guest vector registers.
Booting the same kernel and persistent disk with `arm64.nosme` removed both
failures. The Windows x86-64 test calculated 500500 and wrote its own result
file. This controlled comparison supports the workaround; it does not establish
the precise kernel/Wine defect.

`tools/run-vm.py` now adds `arm64.nosme` by default for HVF. Use `--enable-sme`
only for a controlled regression test, or `--disable-sme` to select the workaround
explicitly with another accelerator. QEMU's HVF `host` CPU does not accept a
`sme=off` CPU property here. The kernel parameter is documented by
[Linux](https://github.com/torvalds/linux/blob/master/Documentation/admin-guide/kernel-parameters.txt).

## OpenXR initialization

The pinned [Proton VR bridge](https://github.com/CachyOS/proton-cachyos/blob/cachyos-11.0-20260703-slr/vrclient_x64/vrclient_main.c)
normally initializes OpenXR registry data only after loading its OpenVR runtime.
Without that path, `wineopenxr` rejects runtime negotiation with
`XR_ERROR_INITIALIZATION_FAILED`, even though native Monado works.

`src/openxr-launcher.c` uses the pinned
[`wineopenxr_init_registry` export](https://github.com/CachyOS/proton-cachyos/blob/cachyos-11.0-20260703-slr/wineopenxr/openxr_loader.c)
to query the actual OpenXR system, Vulkan instance/device extensions and device
IDs. The export's return value alone is insufficient: it can return success
after a native query failure. The launcher deletes prior OpenXR values and
requires all four values to be freshly populated with the expected types
before marking the session ready and starting the application. A mutex prevents
two launcher sessions from modifying the same registry concurrently. State is
invalidated after the child exits.

`tools/run-proton-openxr.sh` selects a separate `compatdata-openxr` prefix.
It does not initialize OpenVR compatibility. The internal export is specific to
the pinned Proton release and must be revalidated when Proton changes.

Build `Containerfile.windows` to obtain `/windows/openxr-launcher.exe`, the
Windows sample, support DLLs and CPU smoke test. In the development guest,
the files live in `/opt/armada-vr/windows`; the three shell wrappers are installed
as `/usr/local/bin/run-runtime`, `run-proton` and `run-proton-openxr`.
The archives and hashes are in `profiles/vr-runtime.json`.

With Monado running and its runtime directory exported:

```sh
run-proton-openxr 'Z:\opt\armada-vr\windows\hello_xr.exe' -g Vulkan
```

The automated sample test owns its Xvfb display and simulated Monado session:

```sh
render-xr "$HOME/.local/state/armada-vr/windows-openxr" \
  run-proton-openxr --test-quit-after 12 \
  'Z:\opt\armada-vr\windows\hello_xr.exe' -g Vulkan
```

`--test-quit-after` sends a real Return key event to the Windows console;
the unmodified sample handles it and exits normally. It is a test option,
not a forced process termination. Proton creates its own Windows console, so
the Linux harness's stdin newline does not reach this sample.

## Evidence and limits

The pinned Wine release also passes `vkGetDeviceProcAddr` to the OpenXR device
creation callback where an instance resolver is required, in
[`win32u_vkCreateDevice`](https://github.com/CachyOS/wine-cachyos/blob/b5f2dc7b5906ef864f83df8fef94c9f539eaad2d/dlls/win32u/vulkan.c).
This produced `VK_ERROR_INITIALIZATION_FAILED` for the Vulkan 2 sample.
`patches/wine/0001-openxr-device-instance-resolver.patch` corrects the source;
its dry application against that commit passes, but a patched Wine build has
not been produced.

The native `src/openxr-procaddr.c` library applies a scoped runtime correction:
it intercepts only `xrCreateVulkanDeviceKHR` acquired through OpenXR's resolver,
recognizes the actual native device-resolver pointer and substitutes the native
instance resolver. Correct callers and other entry points are forwarded. It
retains explicit handles to Wine's privately loaded OpenXR and Vulkan libraries.
The CMake build installs it to `/usr/local/libexec/armada-vr`, and only
`run-proton-openxr` selects it through `LD_PRELOAD` for that process tree.
The unchanged Windows Vulkan 2 sample then passes with 50,171 changed pixels,
309 colors, exit status 0 and a trace confirming both the correction and
successful device creation.

The maintained launcher passed with 49,067 changed pixels, 318 colors and
exit status 0. Captures show distinct left/right views. An independent negative
test with no Monado socket returned 4 and did not create the child program's
result file, despite reusing the previously initialized prefix.

Vulkan 1 evidence is in `output/windows-openxr-maintained/`; Vulkan 2 evidence
is in `output/windows-openxr-vulkan2-maintained/`. The runs used two virtual
CPUs, a 6 GiB VM and a 2 GiB/120% CPU test unit with a 90-second deadline.
Windows execution through ARM64 Proton/FEX and Vulkan/OpenXR rendering are
established for this sample; game compatibility and sustained sessions remain
separate tests. The maintained [controller acceptance](controller-tests.md)
also passes through the Windows loader and Proton/FEX, including both hands'
actions, analog values, disconnect/reconnect and haptic API calls. It covers
headless sessions and simultaneous rendered input. Physical haptic delivery
remains untested.

## Fresh-image acceptance

With the existing desktop build available, run:

```sh
just build-runtime-image
just build-runtime-vm output/runtime-vm
python3 -B tools/run-vm.py --timeout 240 output/runtime-vm
```

`Containerfile.runtime` adds the verified Runtime 4 and Proton archives, Windows
samples and native compatibility library. The runtime belongs to the guest user
so pressure-vessel can lock and hard-link its files. The native Steam SDK link
is present before Steam's first interactive launch. No account data is copied.
`test-windows-xr` first initializes the prefix under Xvfb and requires the actual
Windows CPU test's result file, then runs both OpenXR rendering tests and the
600-sample Windows controller test. It additionally renders real stereo layers
whose captured colors must follow each controller's action state.

`output/runtime-vm-v5/` passes this complete suite from a fresh disk on both
Fedora 7.1 and the Quest-derived QEMU 5.10.246 kernel, with clean shutdown at
approximately 77 and 79 seconds. The `acceptance-fedora.txt` and
`acceptance-quest-kernel.txt` records include every acceptance marker; full boot
logs retain the individual controller transitions and filesystem shutdown.
`output/runtime-vm-v6/` adds simultaneous rendered input, passes on both kernels,
and powers off in approximately 107/108 seconds. The same acceptance filenames
record both complete runs.

`output/runtime-vm-v3/acceptance.txt` records a complete offline run from a fresh
disk: native tracking/stereo, FEX, Windows CPU, Vulkan 1 (45,229 changed pixels,
324 colors), Vulkan 2 (47,266 / 770), and clean shutdown. Earlier image failures
from runtime ownership and a missing `~/.steam/sdkarm64` link are retained under
`output/runtime-vm-v1` and `output/runtime-vm-v2`. The runtime image still does
not include a verified working SteamVR dashboard.
