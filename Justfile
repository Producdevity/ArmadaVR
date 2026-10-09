export engine := env("CONTAINER_ENGINE", "docker")
image := "localhost/armada-vr:vm"

# List the local development commands.
default:
    @just --list

# Build the ARM64 lab, FEX test, and QEMU kernel.
build:
    {{ engine }} build --platform linux/arm64 --target vm -f Containerfile -t {{ image }} .

# Test device-selection and VM-image boundaries without devices or containers.
check:
    python3 -B -m unittest discover -s tests -v
    for script in tools/*.sh; do bash -n "$script" || exit; done
    just --unstable --fmt --check

# Exercise real OpenXR and FEX libraries under ARM64 Linux, capped at 2 GiB.
test:
    mkdir -p output/lab
    {{ engine }} run --rm --network none --memory 2g --memory-swap 2g --cpus 2 --pids-limit 256 --entrypoint bash -v "$PWD/output/lab:/output" {{ image }} -c 'test-xr /output/tracking && render-xr /output/render && test-fex'

# Exercise real OpenXR controller transitions using Monado's virtual driver.
test-input directory="output/input":
    mkdir -p {{ quote(directory) }}
    {{ engine }} run --rm --network none --memory 1g --memory-swap 1g --cpus 1 --pids-limit 128 --entrypoint test-input -v "$(cd {{ quote(directory) }} && pwd):/output" {{ image }} /output/acceptance

# Produce a QEMU-only disk in a new output directory.
build-vm directory="output/vm":
    CONTAINER_ENGINE={{ engine }} bash tools/build-vm.sh {{ quote(directory) }}

# Boot with snapshot writes, verify the checksums, and require passing guest tests.
run-vm directory="output/vm":
    python3 -B tools/run-vm.py {{ quote(directory) }}

# Install the optional native Steam client into a new output directory.
fetch-steam directory="output/steam-client":
    python3 -B tools/fetch-steam.py {{ quote(directory) }}

# Add native Steam UI dependencies to the existing lab image.
build-steam:
    {{ engine }} build --platform linux/arm64 --build-arg LAB_UID="$(id -u)" --build-arg LAB_GID="$(id -g)" -f Containerfile.steam -t localhost/armada-vr:steam .

# Capture a short offline client startup using isolated home and client data.
steam-sandbox:
    mkdir -p output/steam-sandbox
    {{ engine }} run --rm --network none --memory 2g --memory-swap 2g --shm-size 256m --cpus 2 --pids-limit 256 --user "$(id -u):$(id -g)" --entrypoint bash -v "$PWD/output/steam-client:/client" -v "$PWD/output/steam-sandbox:/output" localhost/armada-vr:steam /usr/local/bin/steam-sandbox

# Build a desktop VM from a fresh, verified copy of the native client.
build-desktop directory="output/desktop" client="output/desktop-client":
    python3 -B tools/fetch-steam.py {{ quote(client) }}
    {{ engine }} build --platform linux/arm64 --build-context steam-client={{ quote(client) }} -f Containerfile.desktop -t localhost/armada-vr:desktop .
    VR_VM_IMAGE=localhost/armada-vr:desktop CONTAINER_ENGINE={{ engine }} bash tools/build-vm.sh {{ quote(directory) }}

# Open the persistent development VM in a native QEMU window.
desktop directory="output/desktop":
    python3 -B tools/run-vm.py --desktop {{ quote(directory) }}

# Open the desktop with internet access for Steam sign-in and downloads.
desktop-online directory="output/desktop":
    python3 -B tools/run-vm.py --desktop --network {{ quote(directory) }}

# Check the running VM's advertised SteamVR semaphore-sharing capability.
check-steamvr directory="output/desktop":
    python3 -B tools/vm_control.py {{ quote(directory) }} exec 'runuser -u vr -- /usr/local/bin/vulkan-interop'

# Download the pinned vendor DTS/config subset, preserving modified files.
fetch-kernel profile="quest3":
    python3 -B tools/kernel-source.py fetch {{ quote(profile) }}

# Compile vendor DTB/overlays and apply each overlay offline; no headset access.
build-device-trees profile="quest3":
    python3 -B tools/kernel-source.py build-dt {{ quote(profile) }}

# Authenticate a local OTA with independently collected device certificates.
verify-ota ota certificates device build:
    python3 -B tools/verify-ota.py {{ quote(ota) }} --device-certificates {{ quote(certificates) }} --expected-device {{ quote(device) }} --expected-build {{ quote(build) }}

# Inspect a local boot/vendor_boot copy without accessing a headset.
inspect-boot image:
    python3 -B tools/inspect-boot.py {{ quote(image) }}

# Install the vendor-kernel compiler inside an ARM64 Linux container image.
build-kernel-tools:
    {{ engine }} build --platform linux/arm64 -f Containerfile.kernel -t localhost/armada-vr:kernel .

# Attempt the original vendor configuration in Linux, capped at 6 GiB.
build-vendor-kernel directory="output/kernel/quest3/vendor-build" profile="quest3":
    CONTAINER_ENGINE={{ engine }} bash tools/build-kernel.sh {{ quote(profile) }} {{ quote(directory) }}

# Resolve the Linux userspace configuration separately from the vendor baseline.
configure-headset-kernel directory="output/kernel/quest3/linux-config":
    CONTAINER_ENGINE={{ engine }} bash tools/build-kernel.sh quest3 {{ quote(directory) }} linux-userspace configure

# Compile the vendor kernel with Linux userspace support; still no boot image.
build-headset-kernel directory="output/kernel/quest3/linux-build":
    CONTAINER_ENGINE={{ engine }} bash tools/build-kernel.sh quest3 {{ quote(directory) }} linux-userspace

# Build the vendor kernel with QEMU transports for userspace ABI testing only.
build-kernel-abi directory="output/kernel/quest3/qemu-abi":
    CONTAINER_ENGINE={{ engine }} bash tools/build-kernel.sh quest3 {{ quote(directory) }} qemu-abi

# Boot the QEMU-transport vendor kernel with the existing read-only lab image.
test-kernel-abi kernel="output/kernel/quest3/qemu-abi" directory="output/kernel/quest3/abi-test":
    python3 -B tools/test-kernel-abi.py {{ quote(kernel) }} --output {{ quote(directory) }}

# Exercise real thermal cooling and shutdown with simulated sensors in diskless QEMU.
test-kernel-thermal kernel="output/kernel/quest3/qemu-abi" directory="output/kernel-thermal":
    python3 -B tools/test-kernel-thermal.py {{ quote(kernel) }} --output {{ quote(directory) }}

# Fetch Valve's pinned OpenVR interface used by the stereo frame probe.
fetch-openvr directory="output/openvr-sdk":
    python3 -B tools/fetch-openvr.py {{ quote(directory) }}

# Build the virtual SteamVR driver/probe in ARM64 Linux for either runtime ABI.
build-steamvr-probe sdk="output/openvr-sdk" directory="output/steamvr-probe" target="x86_64":
    bash tools/build-steamvr-probe.sh {{ quote(sdk) }} {{ quote(directory) }} {{ quote(target) }}

# Build the x86-64 OpenXR acceptance sample inside the ARM64 development VM.
build-steamvr-openxr archive="output/downloads/openxr-sdk-b5fd54b.tar.gz" directory="output/steamvr-openxr":
    bash tools/build-steamvr-openxr.sh {{ quote(archive) }} {{ quote(directory) }}

# Test the current user's already-running virtual SteamVR session.
test-steamvr-openxr bundle="output/steamvr-openxr" directory="output/steamvr-openxr-test":
    python3 -B tools/test-steamvr-openxr.py {{ quote(bundle) }} {{ quote(directory) }}

# Add the pinned ARM64 Proton/OpenXR runtime and Windows acceptance samples.
build-runtime-image:
    CONTAINER_ENGINE={{ engine }} bash tools/build-runtime-image.sh

# Produce a fresh QEMU desktop with native and Windows OpenXR boot acceptance.
build-runtime-vm directory="output/runtime-vm":
    VR_VM_IMAGE=localhost/armada-vr:runtime CONTAINER_ENGINE={{ engine }} bash tools/build-vm.sh {{ quote(directory) }}

# Verify offline sensor record decoding against Meta's pinned C UAPI.
test-syncboss directory="output/syncboss":
    CONTAINER_ENGINE={{ engine }} bash tools/test-syncboss.sh {{ quote(directory) }}

# Install the optional software Vulkan driver's compiler in an ARM64 container.
build-mesa-tools:
    {{ engine }} build --platform linux/arm64 -f Containerfile.mesa -t localhost/armada-vr:mesa-tools .

# Compile the pinned Lavapipe synchronization patch and cross-process probes.
build-mesa directory="output/mesa/build":
    CONTAINER_ENGINE={{ engine }} bash tools/build-mesa.sh {{ quote(directory) }}

# Compile the Quest GPU's KGSL userspace driver and run offline Freedreno tests.
build-turnip directory="output/mesa/turnip":
    CONTAINER_ENGINE={{ engine }} bash tools/build-mesa.sh {{ quote(directory) }} turnip

# Build the small ARM64 Podman/FUSE dependency image for Quest VM tests.
build-android-container-tools:
    {{ engine }} build --platform linux/arm64 -f tests/android-container/Containerfile -t localhost/armada-vr:android-container-tools tests/android-container

# Test rootless rootfs/data/APK overlay persistence, isolation and shutdown.
test-android-mounts kernel="output/kernel/quest3/qemu-abi-v10" directory="output/android-mounts":
    python3 -B tools/test-android-mounts.py {{ quote(kernel) }} --engine {{ engine }} --output {{ quote(directory) }}
