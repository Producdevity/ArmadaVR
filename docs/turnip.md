# KGSL Turnip build and offline packaging

The headset graphics backend is native AArch64 Mesa Turnip using Qualcomm's
KGSL kernel interface. FEX is used separately for x86 applications and the
currently available desktop SteamVR runtime. No headset GPU execution or
physical display acceptance is established by these build results.

```sh
bash tools/build-mesa.sh NEW_MESA_DIRECTORY turnip
python3 -B tools/build-headset-root.py KERNEL_BUILD STARTUP_INITRAMFS \
  --firmware QUEST_FIRMWARE --image RUNTIME_IMAGE \
  --turnip NEW_MESA_DIRECTORY --output NEW_ROOT_DIRECTORY
```

The pinned Mesa 26.1.8 archive is checked by size and SHA-256. The build uses an
immutable ARM64 container image, a source cache keyed by profile, patches,
builder and image, and no network during compilation. Existing outputs are
refused. Build and test logs, source identities and artifact hashes are retained.

The four Turnip patches fix fence ownership and wait-any behavior, implement
an opt-in KGSL-to-DRM synchronization bridge, and enable that bridge in the
KGSL-only build. Submission still uses KGSL. The DRM device supplies binary and
timeline synchronization objects; it does not replace the KGSL GPU driver.

The bridge requires `-Dfreedreno-kgsl-drm-sync=true` at build time and
`TU_KGSL_DRM_SYNC` pointing to a suitable DRM device at runtime. The device must
support the required synchronization features. Its actual path and capabilities
must be checked on the target; a numbered render node is not a device identity.

## Correction to earlier build evidence

The September 10 `output/mesa/turnip-drm-v5/` driver compiled with
`MESA_SYSTEM_HAS_KMS_DRM=0` and without `HAVE_LIBDRM`. Mesa's KGSL-only platform
logic disabled libdrm before the target dependency was added. Consequently the
conditional bridge was absent from the final shared library, despite successful
compilation and separate helper tests.

Patch 0004 adds an explicit Meson option that preserves DRM support for this
configuration and makes libdrm required. The builder now requires the actual
KGSL compiler command to enable DRM, the finished ELF to link libdrm, and the
bridge's runtime option to be present in that ELF. Compile and dynamic-link
records are included in the artifact hashes.

`output/mesa/turnip-drm-v7/` passes these checks, all three ASan/UBSan helper
suites and five Freedreno tests. `output/turnip-bridge-audit-v1/` retains the
old compiler command and library identity, verifies rejection of old, damaged,
foreign-architecture and inconsistent bundles, and records loading the corrected
library in the pinned userspace. Vulkan reaches the library and reports no GPU
when no KGSL device is exposed.

The earlier QEMU test of real DRM/software fences remains valid for its exact
helper header, which is unchanged. It covers cross-process opaque descriptors,
future timeline points, merged dependencies and cleanup. The submission-wrapper
suite models KGSL calls. Neither test demonstrates actual GPU completion.

## Root contents

`--turnip` validates the build, source/profile/patch/test identities, artifact
checksums, ELF architecture and installed ICD path. The root receives the
library and ICD under `/opt/armada-vr/turnip/`, plus the build record under
`/usr/share/armada-vr/`. Loading with `RTLD_NOW` inside the assembled userspace
must succeed. After formatting, the builder extracts these files from the ext4
image and compares their hashes with the validated bundle.

Packaging does not select Turnip globally or start a physical VR session.
The existing QEMU software-rendering lab remains available. Headset session
selection, display scanout, GPU execution, tracking/calibration, thermal
operation, exact-device boot acceptance and recovery still require completion.

## Direct headset display remains incomplete

The corrected v7 compile command already enables `VK_USE_PLATFORM_DISPLAY_KHR`
and includes common DRM display WSI. However, `tu_knl_kgsl_load()` still rejects
instances that enable `VK_KHR_display`, sets `master_fd` to `-1`, and uses its
KGSL descriptor as `local_fd`. Turnip's presentation-device check compares the
candidate DRM device with that local descriptor. A separate, correctly selected
SDE display descriptor and its ownership/lifetime handling are still required.

The [DMA-buffer sync-file backport](dma-buf-sync.md) supplies the kernel API
Mesa uses to attach completion fences before presentation. It does not remove
that KGSL display rejection or prove panel presentation. Device access for KGSL
and the system DMA heap, buffer formats/modifiers, display selection and the
physical compositor session remain integration work.
