# ArmadaVR

An experimental ARM64 Linux VR environment and headset board-support project.
Quest 3 and Pico Neo3 Pro Eye are development targets. Neo3 Pro requires
separate board and calibration checks.
There is no validated headset installation image.

The development environment uses Fedora, Armada's FEX packaging, native Monado
and OpenXR, and Proton for Windows applications. Quest work retains Meta's vendor
kernel and board drivers, with Mesa Turnip for KGSL. QEMU provides a separate
virtual target for functional tests.

## Status

| Area | Verified | Remaining |
|---|---|---|
| Quest kernel | ARM64 kernel, 272 modules and 14 device-tree artifacts compile; QEMU checks userspace interfaces and simulated thermal behavior | Exact-device boot and peripheral operation |
| Pico kernel | Vendor/Linux builds with 38 modules; Binder, namespace networking and SELinux denial checks pass in QEMU | Missing board/driver support, full userspace, recovery and exact-device boot |
| Graphics | KGSL Turnip and native Monado builds; offline synchronization and display-selection tests | GPU execution, panel scanout and compositor timing on the headset |
| VR applications | Native and Windows OpenXR stereo samples and virtual controller actions in QEMU | Games, physical tracking, controllers, audio and sustained performance |
| SteamVR | Native Frame ARM64 presentation, cached Library navigation/scrolling with both virtual controllers, and Windows OpenXR on fresh native backends pass in QEMU; translated baseline retained | Complete reproducible image, compositor validation/timing, game and sustained runtime acceptance |
| Android container | Lepton boots Android 11 on the Quest test kernel; Binder, isolation, APK input, default test-APK save lifecycle and networking pass; bounded launcher shutdown is implemented | Matching source-built init shutdown, actual game saves, Android XR, hardware graphics and performance |
| Installation | Authenticated current-build stock OTA, boot-container assembly and integrity checks | Accepted custom boot, exact stock recovery and a tested installation/rollback procedure |

VM results do not establish hardware compatibility or flash readiness. Read the
[device installation requirements](docs/device-installation.md) before planning
any device changes.

## Development

Requirements: Python 3.11+, `just`, an ARM64 Linux container engine, and
`qemu-system-aarch64`. Docker Desktop on Apple Silicon is supported. Device-tree
tests also need `cc`, `dtc` and `fdtoverlay`.

```sh
just check
just build
just test
just build-vm output/vm-new
just run-vm output/vm-new
```

The virtual acceptance tests use software Vulkan and a simulated headset.
Outputs are stored in `output/`; builders require new output directories to
preserve earlier artifacts. Run `just --list` for the supported workflows.

For an interactive desktop with the native ARM64 Steam client:

```sh
just build-steam
just build-desktop output/desktop-new output/desktop-client-new
just desktop output/desktop-new
```

`just desktop-online output/desktop-new` enables user-mode networking for Steam
sign-in and downloads. The persistent QCOW2 overlay references its base disk;
keep them together. A fresh desktop build does not yet include every dependency
used by the recorded SteamVR tests.

## Documentation

- [Kernel and board support](docs/headset-bringup.md), [boot containers](docs/quest-boot-assembly.md), and [offline root filesystem](docs/headset-root.md)
- [Turnip](docs/turnip.md), [Monado](docs/monado.md), and [DMA-buffer synchronization](docs/dma-buf-sync.md)
- [SteamVR](docs/steamvr.md), [native ARM64 runtime](docs/steamvr-arm64.md), [Windows OpenXR](docs/windows-openxr.md), and [controller acceptance](docs/controller-tests.md)
- [VR acceptance milestones](docs/vr-milestones.md), [Steam Frame components](docs/steam-frame.md), and [Android container checks](docs/android-containers.md)
- [Pico firmware and boot access](docs/pico-firmware.md)

Source revisions, archive hashes and build profiles are recorded under
`profiles/`. Build tooling retains upstream license files. Firmware, calibration,
device inventories, downloaded binaries and local investigation journals are
excluded from this repository; headset firmware must be supplied locally.
