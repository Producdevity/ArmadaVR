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
| Native ARM64 SteamVR | Obtainable, verified client/runtime and native composition with matching game bridge | Official Frame repair image supplies SteamVR 2.17.10. Native server, simulated HMD/controllers and real Vulkan sharing pass twice on the Quest QEMU kernel. Graphics preflight correctly refuses the VM's missing display/present-wait capabilities; native compositor presentation and game bridge acceptance remain. See [native acceptance](steamvr-arm64.md) |
| SteamVR interface | Actual stereo dashboard rendering; both hands can point, select, navigate, scroll, use menu bindings and close/reopen it; input focus transfers to and from applications | Stereo dashboard captures and both hands' application-menu, trackpad axes/click/touch, trigger, grip, system, pose and haptic acceptance pass. The installed launcher opens the dashboard automatically. Both hands now pass measured dashboard-overlay pointing, primary selection, click, discrete scroll, menu/Back events and close/reopen, plus Windows application focus transfer. New backend v17 has two passing smooth-scroll repetitions after close/reopen failures and system UI renderer restarts. Backend v19 adds actual stock-root pointer selection and short-press close/reopen for both hands; a one-second test press had crossed the recenter threshold. Stock Library navigation, rendered scroll-content behavior, intermittent renderer exits and earlier nonfinite smooth-scroll events remain. See [details](steamvr.md) |
| Steam Runtime 4 | ARM64 pressure-vessel launch, graphics and OpenXR working across its boundary | Passed native stereo sample, including clean exit through its input pipe |
| Windows compatibility | Correct ARM64 Proton/FEX architecture, Windows program execution, Vulkan rendering | CPU smoke and both Vulkan OpenXR sample bindings passed; game launch pending |
| Windows OpenXR | Windows VR application rendering and receiving controller actions | Monado actions/rendering, SteamVR stereo capture, fresh-prefix launch and both-hand focus/haptic/menu exit pass. Fresh translated backends also pass both hands with the verified FEX browser-cache guard, including a repeat after 120 seconds idle. Exit 247, the CEF failure's cause, a fresh complete image, Windows games and physical output remain unresolved. See [tests](windows-openxr.md) |
| Android compatibility | Actual Lepton boot, APK launch/input, saves, shutdown and graphics/XR | Acquired Android 11 boots on the Quest test kernel with rootless Podman. APK installation, rendered input, baked-package restart, saved counter 2→4 and real error propagation pass. A synthetic Steam compatibility launch retains saves through default cleanup without retention flags; the normal launcher wrapper also passes save restoration and descendant cleanup. Actual game saves, clean init shutdown, Android XR and hardware graphics remain. See [Lepton acceptance](android-containers.md) |
| OpenVR compatibility | OpenVR application rendering and receiving input through the tested runtime | Native x86-64 OpenVR through FEX and actual SteamVR passes frame/controller acceptance; Windows OpenVR games remain untested |
| Interaction and audio | Both controllers' actions, pose changes, recentering, application-level haptic requests and guest audio | Native/Windows actions, rendered interaction, virtual recentering and stereo PipeWire loopback pass. Haptic output, game/spatial audio and physical I/O remain |
| Session reliability | Application exit/relaunch, runtime restart, VM reboot, bounded soak with no unbounded memory growth | Five native controller and five Windows rendered-input restart cycles passed through Monado, plus persistent desktop shutdown/reboot and two fresh interactive SteamVR dashboard launches. Repeated Windows-to-SteamVR testing encountered disk exhaustion, then a shared-image allocation failure after expanding the disk. A clean-backend repeat and fresh-prefix rendering now pass. The October 8 tests retain an unexpected launcher exit 247 and system UI restarts; longer SteamVR soak, that exit and the allocation failure root cause remain |
| Reproducible image | Pinned downloads, supported build/launch commands and captured acceptance results | Fresh offline Runtime 4/Proton image passes CPU, both Windows Vulkan bindings and native/Windows controller tests on two kernels. SteamVR bundle v10 includes the Windows launcher/resolver and is built, pinned, tested and installed with its prior version preserved; a fresh complete SteamVR image remains |

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
