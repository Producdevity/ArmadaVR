# Android platform compatibility

Apply these patches to Mesa 26.1.8 when building with Android platform stubs.
They are separate from the Linux driver patch sets. Retain the upstream
per-file license notices.

The tracing patch restores the inline accessors from the
[Android 10 libcutils header](https://android.googlesource.com/platform/system/core/+/refs/tags/android-10.0.0_r47/libcutils/include/cutils/trace.h)
when `ANDROID_API_LEVEL` is below 30. Newer targets retain the exported accessor
functions. Match Meson's `platform-sdk-version` and the NDK compiler's API level;
setting the Meson version alone does not constrain the compiler's imports.

The build stubs provide link symbols only. Do not install them on a device or
substitute them for the system libraries. The API 29 Turnip test uses the actual
Neo3 Pro Eye Android 10 libcutils and the actual Quest 3 Android 14 libcutils.
Both load and pass GPU buffer/fence and offscreen shader-rendering tests.

The Android test also applies the maintained Turnip KGSL patches. It uses a
private process-local driver, without replacing a system library or selecting a
global driver. See [physical GPU acceptance](../../../docs/turnip.md#physical-android-gpu-acceptance)
for results and the remaining kernel, display and synchronization boundaries.
