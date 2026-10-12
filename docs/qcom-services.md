# Qualcomm services

The Quest USB role/power path depends on ADSP services, including
`msm/adsp/charger_pd`. The vendor 5.10 kernel has a QRTR nameserver and PDR
client, but needs a userspace service locator. This build reuses
[linux-msm/pd-mapper](https://github.com/linux-msm/pd-mapper) and
[linux-msm/qrtr](https://github.com/linux-msm/qrtr) at the commits and archive
hashes in `profiles/qcom-services.json`.

```sh
python3 -B tools/fetch-qcom-services.py SOURCE_DIRECTORY
docker build --platform linux/arm64 \
  --build-context qcom-sources=SOURCE_DIRECTORY \
  -f Containerfile.qcom-services -t localhost/armada-vr:qcom-services .
```

Existing archives are verified and preserved, including when their contents
are rejected. `--verify-only` performs no downloads. The container rechecks
all pinned archives before extraction. It extends the existing Fedora VM
build image; `--build-arg BASE_IMAGE=IMAGE` selects another compatible Fedora
image. Package installation requires network access. Source compilation uses
two jobs with time limits. The resulting image contains the binaries, source
pin manifest, applied patches, licenses, package versions and sanitizer log.
It does not enable the upstream system service or request ADSP boot.

## Pico QRTR name service

Pico's published 4.19 kernel provides QRTR sockets but lacks `net/qrtr/ns.c`.
Unlike the Quest 5.10 kernel, it needs a userspace name service before SSC or
PD mapper discovery can succeed. In the Pico kernel VM, sockets opened
successfully while libqrtr-glib timed out on its initial lookup. Starting
`qrtr-ns -f` restored enumeration of the kernel's registered services.

The build includes `qrtr-ns` from upstream
[v1.2](https://github.com/linux-msm/qrtr/releases/tag/v1.2), the last release
containing that daemon. Only its name-service executable is built from the
additional pinned archive; it links against our current, bounds-checked libqrtr.
The source manifest and separate license are installed with the runtime.
The Pico initramfs builder includes the service only with `--pico-services`.
This requires the pinned Pico kernel, a verified Pico firmware bundle and a
matching private systemd runtime. The installed service additionally requires
the exact kernel command-line token `armada.pico=qrtr` and a Kona device tree.
It refuses an existing name service, waits for successful discovery before
notifying systemd, and stops its own daemon on exit. Startup failure requests
power-off. It does not start a DSP or load sensor firmware.

The initramfs service stops before `initrd-switch-root.target`. Restarting QRTR
in the real root and reconnecting its clients still need integration. Enabling
the initramfs service alone does not provide continuous sensor transport.

The packaged daemon and current libqrtr pass all 15 upstream libssc cases in
seven suites on the exact physical-configuration Pico kernel in diskless QEMU.
The test waits for actual service-400 registration before starting each client.
It covers missing and unsupported sensors, attributes, samples, synchronous
discovery and accelerometer mount matrices. QRTR and the name service are real;
the DSP sensors are mocked. The VM shuts down normally. Full logs, the loaded
kernel hash and the preserved failed attempts are under
`output/pico-ssc-transport-20261012-v1/research/qrtr-vm-v8/`; `verification.json`
uses decoded printk records rather than scanning binary memory as text.

The boot helper passes seven lifecycle cases on the same diskless Pico kernel:
wrong platform, inexact command-line token, existing broker, normal readiness
and shutdown, daemon exit, stopped-daemon timeout and cleanup, and restart.
The actual systemd manager also accepts its readiness notification, stops and
restarts it, and stops it before a modeled switch-root target. Evidence is under
`output/pico-qrtr-startup-20261012-v1/`. The virtual board identity is modeled;
these checks do not perform a real root pivot or execute physical DSP firmware.
With a deliberately stopped daemon, the real manager records the discovery
timeout, fails the unit and powers off the VM. The full run takes about 16
seconds. The earlier failed log assertions remain alongside the final
`manager-failure-v3` result.

## Local fixes

Real malformed QMI requests exposed missing packet bounds checks in libqrtr.
The local patch checks header and TLV lengths, scalar/array payload sizes and
string capacity before copying. Sanitizer tests also reproduce an out-of-bounds
header read in the unmodified pinned library. Those tests run against the
patched library source during the container build. This is a targeted fix,
not a complete audit of the upstream JSON or QMI parsers.

A second patch makes PD mapper report the actual missing firmware directory.
Upstream's failure message referenced an unset buffer when no custom firmware
search path was configured.

## Virtual protocol acceptance

```sh
python3 -B tools/test-qcom-services.py QEMU_KERNEL_BUILD QEMU_INITRAMFS_BUILD \
  SOURCE_DIRECTORY --output NEW_TEST_DIRECTORY
```

The test requires a matching, checksum-verified QEMU kernel and firmware-bearing
initramfs. Physical-headset kernel variants are refused. It uses an immutable
container ID, verifies the installed source manifest and both patches, and
compiles the probe against the real libqrtr. Compilation runs without network,
with a 60-second limit, two CPUs and 512 MiB. A separate diskless, networkless
QEMU run is limited to 45 seconds and 1 GiB.

Inside QEMU, the fixture checks PID 1 and the `linux,dummy-virt` device-tree
identity before manipulating its modeled remoteproc directory or powering off.
Only remoteproc firmware-name enumeration is modeled. The vendor kernel QRTR
sockets, nameserver, PD mapper, packaged service JSONs and QMI replies are real.
No ADSP firmware executes.

Each of two mapper starts receives 96 requests: valid `tms/servreg` queries,
unknown-service queries and malformed TLVs. Replies must contain the exact
four unique ADSP domains, including `charger_pd`, with QMI instance 74 and
matching transaction/server identities. The request encoder follows the vendor
PDR client's string descriptor; the upstream mapper's decode descriptor cannot
be reused unchanged for encoding. A missing firmware directory must cause an
exit with the correct path diagnostic. Both cases end in controlled power-off.

September 11 evidence is retained under `output/qcom-services-build-v3/` and
`output/qcom-services-test-v6/`: the patched build, sanitizer checks, 192 QMI
requests, mapper restart, missing-map exit and power-off pass. Earlier test
failures remain in versions 1–4; version 5 passed protocol checks but exposed
the error-message defect fixed for version 6.

## Remaining boot work

The service mapper is built and protocol-tested. It still needs deliberate
initramfs service ordering, bounded ADSP startup and handoff to the real root
filesystem. These tests do not establish PMIC-GLINK/UCSI readiness, DSP
authentication, USB-C negotiation or physical USB storage operation. Exact-unit
recovery and acceptance of a custom kernel remain unresolved. This work does
not establish flash readiness.
