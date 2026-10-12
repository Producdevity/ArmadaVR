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

Pico boot arguments must also retain `service_locator.enable=1` from the stock
boot image. The pinned 4.19 driver's default is zero; its initialization returns
`ENODEV` while disabled. The shared boot-argument helper's `pico_services=True`
option emits both this parameter and `armada.pico=qrtr`, preserving the
read-only/volatile-root protections and refusing mixed Quest/Pico startup.
Two diskless boots of the exact physical-configuration kernel verify the
service-locator sysfs parameter reads zero without the option and one with it.
This enables the kernel interface; a running mapper and physical DSPs still
need separate verification.

With a combined Pico GPU/DSP bundle, the builders also install a supervised
PD mapper and checksums for the five validated service maps. The mapper unit
requires QRTR, checks the Kona identity, command-line opt-in, enabled kernel
locator and map hashes, then passes the maps explicitly. It refuses an existing
mapper and reports readiness only after service 64, version 1, instance 1 is
registered. Startup is bounded; failure requests power-off. Stopping QRTR stops
the mapper, and the initramfs units stop before switching roots. GPU-only
bundles retain QRTR without enabling a mapper. Neither service starts a DSP.

The production mapper helper passes 18 lifecycle checks and 256 QMI requests
on the exact Pico kernel in diskless QEMU. These cover platform and token
refusal, a disabled locator, missing or changed maps, unavailable QRTR,
duplicates, owned-child shutdown, daemon exit, timeout cleanup and restart.
The real systemd manager passes another 384 requests across startup/restart
and verifies dependency shutdown and stopping at the switch-root target.
The physical board identity is modeled only inside this isolated fixture.
Evidence, including failed fixture attempts, is under
`output/pico-pd-startup-20261012-v1/`.

The corresponding offline root installs 17 service/runtime files and three
links. Exact ext4 readback, filesystem checks and relocations using that root's
own native loader and libc pass. The source root remains unchanged. These
checks establish packaging and service behavior; they do not establish a
physical DSP boot, display output or a recoverable firmware installation.

The initramfs service stops before `initrd-switch-root.target`. When the selected
initramfs includes Pico services, the root builder installs the corresponding
root-stage service, native QRTR binaries, library, source pins and licenses.
It requires the matching QCOM build image and verifies the installed files and
service links by reading them back from ext4. The root-stage unit also requires
`armada.pico=qrtr`, so packaging it does not authorize a physical boot.

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

An actual systemd switch-root test stops the initramfs broker, changes to a
separate RAM root, and verifies that a new root-stage broker becomes ready while
the old process is gone. Both stages use the production units and helper. This
test explicitly stops the first unit before requesting the root switch; it does
not validate the complete physical storage or dracut boot sequence. The
`output/pico-qrtr-root-20261012-v1/pivot-vm-v2/` result preserves the kernel hash
and before/after process IDs. Native relocation checks also pass using the
updated full root image's own loader and libc. Reconnecting sensor clients and
physical DSP startup remain outstanding.

## Local fixes

Pico's selected kernel has `CONFIG_MSM_PIL=y` and no remoteproc framework. The
mapper's upstream automatic discovery therefore cannot find its service maps.
The `explicit-service-maps` patch adds repeatable `-f SERVICE_MAP` arguments,
deduplicates repeated paths and rejects invalid options or unreadable/malformed
maps before opening QRTR. With no `-f`, automatic remoteproc discovery remains
available. The build installs the patch alongside the pinned source manifest;
Quest service builds also verify that identity.

On the exact Pico kernel in diskless QEMU, the packaged mapper answers 256
requests across two explicit-map starts using all five authenticated Pico
declarations. Responses identify ADSP instance 74, CDSP 76 and SLPI 90; the
audio service resolves only to `msm/adsp/audio_pd`. Duplicate file arguments do
not duplicate domains. Unknown services and malformed QMI requests are also
checked. Another 192 requests verify baseline and patched automatic discovery
with modeled remoteproc entries and four Quest-domain fixtures. Invalid CLI
and map inputs fail before service publication. Evidence is under
`output/pico-pd-mapper-20261012-v1/vm-v3/`; no DSP executes in this test. The
firmware-read fix is retested with the packaged runtime in `vm-v5/`.

Real malformed QMI requests exposed missing packet bounds checks in libqrtr.
The local patch checks header and TLV lengths, scalar/array payload sizes and
string capacity before copying. Sanitizer tests also reproduce an out-of-bounds
header read in the unmodified pinned library. Those tests run against the
patched library source during the container build. This is a targeted fix,
not a complete audit of the upstream JSON or QMI parsers.

The firmware-attribute read patch keeps the signed read result, rejects empty
or potentially truncated values, and reserves space for the terminator. The
unmodified source writes outside its buffer after an I/O error or full-buffer
read; an empty value accidentally scans the firmware root. Actual-source
sanitizer regressions cover all three failures and a valid firmware name.
The runtime build runs these checks and installs the patch identity. Evidence
is retained under `output/pico-pd-mapper-20261012-v1/read-checks/`.

A separate patch makes PD mapper report the actual missing firmware directory.
Upstream's failure message referenced an unset buffer when no custom firmware
search path was configured.

## Virtual protocol acceptance

```sh
python3 -B tools/test-qcom-services.py QEMU_KERNEL_BUILD QEMU_INITRAMFS_BUILD \
  SOURCE_DIRECTORY --output NEW_TEST_DIRECTORY
```

The test requires a matching, checksum-verified QEMU kernel and firmware-bearing
initramfs. Physical-headset kernel variants are refused. It uses an immutable
container ID, verifies the installed source manifest and applied patches, and
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

The Pico service mapper is packaged and its startup ordering is tested.
Bounded DSP startup and verification on the physical root remain outstanding.
These tests do not establish PMIC-GLINK/UCSI readiness, DSP
authentication, USB-C negotiation or physical USB storage operation. Exact-unit
recovery and acceptance of a custom kernel remain unresolved. This work does
not establish flash readiness.
