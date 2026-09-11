# Qualcomm service mapper

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
both pinned archives before extraction. It extends the existing Fedora VM
build image; `--build-arg BASE_IMAGE=IMAGE` selects another compatible Fedora
image. Package installation requires network access. Source compilation uses
two jobs with time limits. The resulting image contains the binaries, source
pin manifest, applied patches, licenses, package versions and sanitizer log.
It does not enable the upstream system service or request ADSP boot.

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
