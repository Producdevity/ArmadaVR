# Lepton 2.8.14 integration

These patches target the Steam-delivered launcher `2.8.14` / image `2.8.11`,
app 3029110 build 25257944. They are not patches for public source v3.0.5.
Retain the delivered licenses and apply them only to a separate execution copy.
The original acquired payload, rootfs, sysbake and xattrs must remain preserved.

Expected original launcher files:

| File | SHA-256 |
|---|---|
| `liblepton/properties.sh` | `ec6af956c92e93092bee71998c2fcbda9c0e25385e8c95f30cb631927ef6fa9d` |
| `liblepton/mounting.sh` | `4e53b2762198ef1350d80dcdfcabb3dcdf084db6cc2887ed94555a59321ab4f3` |
| `liblepton/liblepton.sh` | `26c7c56106ed43c28ce0cc71ae62a859d6c55770506e8044326becac6b1ca073` |
| `images/rootfs_overlay/system/bin/cmd` | `66337f13fc6fd1d68d07ae5ea174dd7120d8800113a4e52ebdc33b639d6b443e` |

From the execution-copy directory, check the original hashes and run
`git apply --check` with all four patches before applying them in numbered order. Refuse another
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
descendant cleanup. Actual game saves, clean Android init shutdown and Android
XR remain separate acceptance gates.

See [actual Android acceptance](../../docs/android-containers.md#acquired-lepton-and-actual-android-boot)
for measured results and remaining application, shutdown, XR and hardware limits.
