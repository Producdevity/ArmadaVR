# Lepton 2.8.14 integration

These patches target the Steam-delivered launcher `2.8.14` / image `2.8.11`,
app 3029110 build 25257944. They are not patches for public source v3.0.5.
Retain the delivered licenses and apply them only to a separate execution copy.
The original acquired payload, rootfs, sysbake and xattrs must remain preserved.

Expected original launcher files:

| File | SHA-256 |
|---|---|
| `liblepton/utils.sh` | `1f3f0d6383739e0d86ceeeb4b94b74b3c161be6756f6d3a318424716f54a30b3` |
| `lepton` | `f892362538effbefbbd70355cf1f659582ad78fec214f0e3eaf8adcc802a0f45` |
| `liblepton/properties.sh` | `ec6af956c92e93092bee71998c2fcbda9c0e25385e8c95f30cb631927ef6fa9d` |
| `liblepton/mounting.sh` | `4e53b2762198ef1350d80dcdfcabb3dcdf084db6cc2887ed94555a59321ab4f3` |
| `liblepton/liblepton.sh` | `26c7c56106ed43c28ce0cc71ae62a859d6c55770506e8044326becac6b1ca073` |
| `images/rootfs_overlay/system/bin/cmd` | `66337f13fc6fd1d68d07ae5ea174dd7120d8800113a4e52ebdc33b639d6b443e` |
| `liblepton/baking.sh` | `e5d11e2c3027f1a067225fd6141bbd95fe527734290cac38c722ed42d056c4b5` |

From the execution-copy directory, check the original hashes and run
`git apply --check` with all fourteen patches before applying them in numbered order. Refuse another
launcher version rather than applying with fuzz or assuming compatibility.

The shared-memory patch uses memfd because this launcher does not mount ashmem
in Android's private `/dev`. SDK 30's libcutils also requires an eligible VNDK
version and working `MFD_ALLOW_SEALING` / `F_SEAL_FUTURE_WRITE`; the delivered
VNDK 30 image and tested Quest kernel pass the real framework allocations.

The network patch removes exactly Podman's `/proc/sys` read-only entry.
Android's private network namespace needs writable IPv6 configuration even for
the launcher's static IPv4 configuration. [Podman 5.8.7 matches complete protected
paths](https://github.com/containers/podman/blob/v5.8.7/pkg/specgen/generate/config_linux.go#L172-L184),
so `/proc/sys/net` cannot override the protected parent. Other default masks,
private user/network namespaces and rootless credentials remain in effect.
Do not bind the host's `/proc/sys/net` or use `unmask=ALL` / privileged mode.
The VM test confirms distinct host/container namespaces and denied access to
the nonnamespaced `kernel.randomize_va_space` sysctl.

The installation patch pairs ADB's supported `--no-streaming` mode with a fix
to the delivered overlay's `cmd` hook. The old hook returns zero after a failed
package operation because its final conditional has no matching branch. Capture
the command's status, restore Vulkan layer mounts on installation failure, and
return the original failure. Other failing `cmd` operations also retain their
status. The nonstreaming option alone is unsafe with the original hook.

Before using this installation path, verify the exact guest ADB endpoint
advertises `shell_v2` through `adb -P <private-server-port> -s <guest-endpoint> features`.
The tested host uses android-tools 37.0.0 against the delivered SDK 30 image.
Streaming installation fails when the old Android linker prints an unknown
vDSO BTI-tag warning before the protocol's `Success` token. Nonstreaming uses
the independent shell-v2 exit packet; warnings remain visible and invalid APKs
still fail. This does not repair or replace the Android linker.

The timestamp patch preserves the original APK modification time when copying
it into a temporary prefix. Lepton compares that time with `packages.xml` to
decide whether the app is baked. A newly timestamped copy selects an unbaked
mount/property configuration; the package file can become newer during boot,
causing the later installation check to skip both installation and the launch
release. Preserving the source time makes an unchanged APK's second launch
select its actual baked package path.

The actual two-launch test auto-starts the APK, records two taps, then restarts
with its saved counter of 2 and records two more taps. Both launchers exit zero
after deliberate Android force-stop. Invalid installation returns 255, an
unknown service returns 20 and shell exit 7 remains 7. The standalone test
retains its context/data with diagnostic flags. A separate synthetic Steam
compatibility launch also restores the counter through default cleanup without
retention flags, removing its temporary prefixes, work directories and settings.
The normal launcher process-group wrapper also passes save restoration and
descendant cleanup. Actual game saves and Android XR remain separate acceptance
gates.

The shutdown patch gives Android 15 seconds to stop after `/system/bin/reboot -p`
instead of immediately issuing `podman stop -t 0`. GNU `timeout` bounds the wait
and kills a stuck wait process after another two seconds. A failed or expired
wait retains the forced-stop fallback. A nonexistent context needs no shutdown.
The patch does not add `CAP_SYS_BOOT` or change container isolation.

The baked-data patch restores Android's depot attributes into a fresh copy below
the launch prefix. It preserves the installed files and their timestamps, allows
a shared read-only installation, and gives the copy the existing prefix cleanup
lifetime. Both the prefix and persistent Android data require user extended
attributes; on the tested 5.10 kernel, use ext4 instead of tmpfs. The optional
debug-filesystem patch omits debugfs/tracing binds when those host directories
are absent, without changing their behavior when present.

The runtime-ownership patch removes Podman's recursive ownership adjustment from
read-only binds of the installed OpenVR paths file and Steam client libraries.
These shared files are already readable; attempting to change their ownership
fails on a read-only installation. Generated files in the user's launch prefix
retain their existing ownership handling.

The storage-context patch preserves `HOME`, the XDG directories and the two
Podman configuration paths when the launcher attaches to Android. Its otherwise
empty environment previously selected a different container database. The
Android command search path remains isolated; unrelated host variables are not
forwarded.

The short-session patch preserves baked app data when a session ends before
30 seconds. The original launcher treated every short session as a failed start
and erased that data, including local app preferences, before stopping Android.
The patch retains diagnostic log capture and normal shutdown. The virtual APK
regression reproduces the lost restart state, then verifies two short sessions
with the patch: the saved input counter survives and advances from two to four,
both Android and Lepton exit successfully, and temporary prefixes are removed.

There is a separate defect in the delivered Android init: its terminal
non-reboot-capable branch calls `exit(0)`, which runs a joinable static thread's
destructor and aborts after synchronization. The launcher patch alone cannot
repair that binary. The [Android init source patch](../android-init/README.md)
changes that terminal call to `_exit(0)`. A complete source-built init now passes
the baseline comparison, APK input/save/restart and Android GLES/Vulkan OpenXR
tests in the VM. Both corrections are needed for clean default cleanup.

The final cleanup patch makes directories owner-searchable before discarding
baked data. Android can leave `ssh/empty` at mode `0600`; a FUSE overlay marker
inside it then survives `rm -rf` with a permission error. The permission change
applies only when baked data is being deleted. Explicit retention remains
unchanged, a symlink at the baked-data root is refused, and recursive chmod
does not follow symlinks encountered inside the tree.

The launch-timeout patch backports `LEPTON_APP_LAUNCH_TIMEOUT` from Valve's
public v3.0.5 launcher, retaining the delivered 60-second default. CPU-emulated
Android tests can select a longer deadline without changing production timing.
The installed 2.8.14 launcher otherwise ignores this setting and can report an
app-start failure while the activity is still starting.

The emulated-storage patch sets `external_storage.sdcardfs.enabled=false`.
Android otherwise selects sdcardfs when the host kernel advertises it, although
that filesystem cannot mount inside the rootless user namespace. The delivered
Android image already enables its MediaProvider FUSE path. This property does
not remove sdcardfs from the kernel or grant the container additional privileges.
Pico also needs the tagged-page-pinning correction for Android's FUSE buffers;
successful mount creation alone does not establish usable app storage.

The logging patch keeps the log reader alive through process-group teardown.
The delivered launcher terminates its own logging pipeline, then prints another
message into that pipe. This races with reader exit and can kill the launcher
with SIGPIPE after Android has shut down successfully. Only the logging
subprocess ignores INT/TERM; it drains until its writers close their descriptors.
Container and application signal handling are unchanged.

The reconstructed engineering build is separate from the original acquired
image; Valve's exact resolved image manifest and release integration remain open.

See [actual Android acceptance](../../docs/android-containers.md#acquired-lepton-and-actual-android-boot)
for measured results and remaining application, shutdown, XR and hardware limits.
