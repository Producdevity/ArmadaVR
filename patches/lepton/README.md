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

From the execution-copy directory, check the original hashes and run
`git apply --check` with both patches before applying them. Refuse another
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

See [actual Android acceptance](../../docs/android-containers.md#acquired-lepton-and-actual-android-boot)
for measured results and remaining application, shutdown, XR and hardware limits.
