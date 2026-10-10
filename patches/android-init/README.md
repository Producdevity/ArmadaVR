# Android 11 container init

This Apache-2.0 patch corrects Android init's terminal shutdown path when
`CAP_SYS_BOOT` is absent. It belongs to the Android image build, separately
from the [Lepton launcher patches](../lepton/README.md). Retain the upstream
Android copyright and license notices when building or distributing init.

The verified source base is LineageOS `android_system_core` commit
`92420679ffbd50b79d76525085b21278f8497037`, with the SDK 30 `system/core`
patches from Waydroid commit `e2619850d7bfd9bf88d2c8a927ea221f6deb9bd7`
followed by Valve Lepton commit `8be10c8a86ed2a6de1c6a2d6587994e957a35515`.
All 31 upstream patches apply without fuzz. Apply this patch last; reject
source changes rather than assuming compatibility.

The patched `init/reboot_utils.cpp` SHA-256 is
`274642ade429ddabf7c76eca0401f3a58fb2ef5c5f61b5d4d7e36069a7de0927`.
No binary instruction replacement is needed for the source-built candidate.

The offline build uses Android 11 Bionic/libc++ headers, generated SDK 30
protobuf/sysprop sources, static protobuf-lite, the delivered image's shared
libraries and NDK `28.2.13676358` targeting API 30. Both comparison binaries
retain PIE, RELRO, a nonexecutable stack, strong stack protection, FORTIFY
and signed-integer-overflow traps. Both have the original init's 20 shared
library dependency names. Build flags include userdebug init settings; this
is an engineering container build, not a release image or physical-device init.

The original delivered init and all acquired image files remain unchanged.
The build reconstructs a compatible source closure; it does not establish
Valve's exact resolved build manifest or reproduce the delivered binary.
The source locks, build commands, debug binaries, negative comparison and
VM acceptance evidence are retained in `output/init-source-20261010-v1`.

See [Android shutdown acceptance](../../docs/android-containers.md#android-shutdown-comparison)
for the baseline crash, source-built correction, application restart and
remaining image integration requirements.
