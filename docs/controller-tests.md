# Virtual controller acceptance

`test-input OUTPUT` starts Monado's official remote driver in a temporary
configuration, with its own IPC directory and an ephemeral TCP port. Run it
inside the development VM or an isolated Linux container. The client connects
only to loopback; no physical driver is selected. The automated VM has no
network interface. All processes have finite deadlines and are cleaned up.

The protocol matches installed Monado
`25.1.0^20260820git01c1f6b-1.fc44`, source
[`01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2`](https://gitlab.freedesktop.org/monado/monado/-/tree/01c1f6b23ab73c1459c8f4cfd2eb16d5142214c2).
The wire layout comes from `src/xrt/drivers/remote/r_interface.h`: the native
376-byte `mndrmt3` packet contains a head and two 120-byte controller records.
The harness receives both server templates and checks their protocol header
before changing fields. The configuration needs `view_count: 2` as well as
version and port; omitting view count makes this release silently use port 4242.
See `u_config_json_get_remote_settings` and `target_builder_remote.c` in that
same revision. Revalidate the layout and settings when Monado changes.

The real OpenXR probe binds each Index controller's grip pose, trigger click,
trigger value and haptic action. Over 600 samples, it requires both controllers
to press and release, reach trigger value 0.75, become inactive, and become
active again. It also checks stereo views, valid poses, head movement, monotonic
timestamps and orderly session exit. The initial run on Quest-derived
Linux 5.10.246 passed with 600 valid stereo samples and 541 tracked samples per
controller; the deliberate one-second disconnect accounts for the rest.

The clean runtime image repeats this acceptance on both Fedora's 7.1 kernel and
the Quest-derived 5.10.246 QEMU kernel. Five successive runtime start/test/stop
cycles also passed. After each cycle's process cleanup, the test cgroup retained
0.88–0.97 MiB; this is residual accounting, not the peak during rendering.
The bounded soak used a 700 MiB cap and a 130-second deadline. Its log is
`output/steamvr-session/diagnostics-kernel-v4/openxr-input-soak.txt`.

Both haptic start/stop requests succeed at the OpenXR API. The remote driver's
output callback is a default stub, so this result establishes API handling only.
It does not establish delivery to a haptic device. SteamVR controller actions
and game audio remain separate tests.

`test-input OUTPUT --windows` runs the same C++ action probe as a Windows x86-64
executable through the pinned Khronos loader, ARM64 Proton/FEX and native Monado.
Windows uses `XR_KHR_win32_convert_performance_counter_time`; native Linux uses
the timespec conversion extension. The harness waits for the application's
explicit ready marker before changing controls. Windows writes its results to
an explicit file because its Wine console does not inherit the Linux output pipe
reliably. The test does not substitute a mock OpenXR implementation.

Fresh image `output/runtime-vm-v5/` passes both native and Windows action tests
on Fedora 7.1 and Quest-derived 5.10.246. Each application checks 600 stereo
samples and every required transition for both hands. These tests use Monado's
headless compositor; the separate Vulkan 1 and Vulkan 2 tests establish rendered
stereo output.

The combined test now exercises both paths together:

```sh
xvfb-run -a --server-args='-screen 0 1280x800x24 -noreset' \
  test-input OUTPUT --render
xvfb-run -a --server-args='-screen 0 1280x800x24 -noreset' \
  test-input OUTPUT --windows --render
```

The probe creates real Vulkan 2/OpenXR swapchains and submits a stereo projection
layer on each rendered frame. Left/right trigger actions change the corresponding
eye's color. Four compositor captures must show the initial colors, each separate
press, and release. The checker rejects stale frames, unexpected colors and
reversed eye ordering. It also requires all pose/action/disconnection checks and
normal session exit. The ready marker is emitted only after the session gains
input focus.

Both maintained binaries pass on Quest-derived QEMU Linux: 600 samples,
599 submitted projection frames, and approximately 90 pressed frames per hand.
Validation runs confirm the actual native Vulkan instance/device validation layer
loaded in both paths, with no Vulkan validation errors. Images, logs and hashes
are under `output/rendered-input-validation-native/` and
`output/rendered-input-validation-windows/`. The compositor captures were visually
inspected as well as checked numerically. These are functional test patterns,
not game or headset-performance results.

The logs retain Monado's unimplemented remote-haptic callback messages and a
space-destruction diagnostic during teardown. Applications and the service exit
successfully; those runtime diagnostics have not been resolved. The test uses a
disabled D-Bus endpoint to avoid an unrelated desktop portal mounting into its
temporary runtime directory; an absent filesystem path breaks Runtime 4's bind
validation, so it uses the existing `/dev/null` path.

An additional native/Windows diagnostic moves the virtual head 35 cm along X,
then invokes the installed `monado-ctl -c` against only that test's private IPC
socket. Both applications receive one `XrEventDataReferenceSpaceChangePending`
for LOCAL space and observe their view-space center change from 0.35 m to 0 m
while stereo/controller tests continue to pass. The runtime sets `poseValid`
false, so the test does not interpret `poseInPreviousSpace`; it verifies the
coordinates through subsequent `xrLocateViews` calls. This is allowed by the
[OpenXR event specification](https://registry.khronos.org/OpenXR/specs/1.0/html/xrspec.html#XrEventDataReferenceSpaceChangePending).
The first diagnostic incorrectly required that optional pose and failed; its
correction preserves the event and actual coordinate requirements. Evidence:
`output/recenter-native-v2/` and `output/recenter-windows-v2/`, with temporary
test sources in `.agents/`. This supplemental check is not yet part of the
fresh-image boot suite.

Five consecutive Windows rendered-input runs also pass on the Quest-derived
kernel after reboot. Each starts its own Xvfb/Monado session and relaunches
Proton with the existing test prefix. The 2 GiB cgroup reports zero memory
limit/OOM events; post-cycle accounting stays around 526 MiB. `memory.peak`
is unavailable on this kernel, so no peak-memory claim is made. All five
application logs and capture measurements are preserved in
`output/windows-rendered-soak-v1/`. This bounded test does not establish
SteamVR, game or long-session stability.

Monado documents this driver for
[hardware-free development and conformance testing](https://monado.freedesktop.org/developing-with-monado.html).
