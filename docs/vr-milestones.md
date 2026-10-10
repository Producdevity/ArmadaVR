# VR software milestones before hardware installation

Physical flashing waits until the virtual VR software work below is
complete and reviewed. The complete headset kernel and driver port remains part
of the goal. A successful VM run cannot validate a headset bootloader,
recovery path, panel, tracking camera, calibration, radio or thermal control.
Those remain additional prerequisites for a physical installation.

| Milestone | Required evidence | Current status |
|---|---|---|
| Native OpenXR | Stereo rendering, valid HMD/controller poses, keyboard-controlled movement | Passed in ARM64 QEMU |
| SteamVR OpenXR | OpenXR projection layers reaching SteamVR's compositor | Passed through FEX with valid stereo poses, actual presentations, captured eye colors and clean exit; sample only |
| ARM64 Steam client | Rendered client and persistent desktop session | Persistent authenticated client session; game launch pending |
| Native ARM64 SteamVR | Obtainable, verified client/runtime and native composition with matching game bridge | Official Frame SteamVR 2.17.10 passes native software stereo presentation, virtual controllers and Windows OpenXR through FEX/Proton on fresh backends. Maintained launchers and checksum guards are implemented. Compositor validation/timing, complete image assembly and physical presentation remain. See [native acceptance](steamvr-arm64.md) |
| SteamVR interface | Actual stereo dashboard rendering; both hands can point, select, navigate, scroll, use menu bindings and close/reopen it; input focus transfers to and from applications | The translated baseline passes authenticated Home/Library navigation and rendered scrolling with both hands. The native client also passes cached Library navigation and rendered scrolling. Virtual input/haptic checks, dashboard selection and Windows focus/menu-exit tests pass. Sustained renderer reliability and physical interaction remain; earlier failures are preserved. See [translated](steamvr.md) and [native](steamvr-arm64.md) evidence |
| Steam Runtime 4 | ARM64 pressure-vessel launch, graphics and OpenXR working across its boundary | Passed native stereo sample, including clean exit through its input pipe |
| Windows compatibility | Correct ARM64 Proton/FEX architecture, Windows program execution, Vulkan rendering | CPU smoke and both Vulkan OpenXR sample bindings passed; game launch pending |
| Windows OpenXR | Windows VR application rendering and receiving controller actions | Native and translated SteamVR backends pass fresh-prefix stereo rendering, both-hand trigger/haptic input, dashboard focus transfer and normal menu exit. Monado sample checks also pass. Earlier exit 247 remains unexplained; complete image assembly, Windows games and physical output remain unverified. See [tests](windows-openxr.md) |
| Android compatibility | Actual Lepton boot, APK launch/input, saves, shutdown and graphics/XR | Android 11, Binder/isolation, APK rendering/input, default test-APK save retention and network provisioning pass on the Quest test kernel. Bounded launcher shutdown is implemented. A diagnostic init exit change passes repeated shutdown/restart, but matching source-built init acceptance, actual game saves, Android XR and hardware graphics remain. See [Lepton acceptance](android-containers.md) |
| OpenVR compatibility | OpenVR application rendering and receiving input through the tested runtime | Native x86-64 OpenVR through FEX and actual SteamVR passes frame/controller acceptance; Windows OpenVR games remain untested |
| Interaction and audio | Both controllers' actions, pose changes, recentering, application-level haptic requests and guest audio | Native/Windows actions, rendered interaction, virtual recentering and stereo PipeWire loopback pass. Haptic output, game/spatial audio and physical I/O remain |
| Session reliability | Application exit/relaunch, runtime restart, VM reboot, bounded soak with no unbounded memory growth | Five native controller and five Windows rendered-input restart cycles passed through Monado, plus persistent desktop shutdown/reboot and two fresh interactive SteamVR dashboard launches. Repeated Windows-to-SteamVR testing encountered disk exhaustion, then a shared-image allocation failure after expanding the disk. A clean-backend repeat and fresh-prefix rendering now pass. The October 8 tests retain an unexpected launcher exit 247 and system UI restarts; longer SteamVR soak, that exit and the allocation failure root cause remain |
| Reproducible image | Pinned downloads, supported build/launch commands and captured acceptance results | Offline Runtime 4/Proton images pass native/Windows sample tests on two kernels. Maintained native SteamVR presentation and Windows launchers pass from hash-verified bundles. A fresh complete image containing all tested SteamVR/client dependencies remains |

The next tests use redistributable samples and local prefixes without requiring
a Steam account. A direct Proton launch does not count as Steam library launch
proof. Software Vulkan is suitable for functional checks, not headset frame-time
or power measurements. Simulated haptics can establish API handling, not motor
operation. Each result must retain those limits.

Menu and trackpad support are release requirements. Passing virtual input
components is only one part: both hands must work in the dashboard and
applications, and the physical controller mapping must preserve those usable
actions. Missing inputs or failed navigation cannot be deferred past flash
readiness.

Do not add a hardware flash target or generate a Quest/Pico installation image
as a shortcut around an unfinished milestone. Device inventory and source
research may proceed independently; any future hardware work needs an explicit
plan for the exact device and firmware, with a verified recovery procedure.
