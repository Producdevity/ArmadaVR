# Quest early storage and ADSP dependencies

The optional Quest initramfs service now packages the verified ADSP firmware and
[service mapper](qcom-services.md), requests vendor ADSP startup once, and waits
for the PMIC channel, UCSI port and USB host role before releasing dracut's root
search. Its startup and root-handoff lifecycle are tested with real systemd,
vendor modules and QRTR in QEMU. The ADSP and USB state in those tests is modeled;
physical USB root operation remains unverified.

## Dependency traced in the vendor source

The source below is Meta commit `dbc2bc83a2b673f5d1c24ee9dadb0cd708ffda3f`, used
by `output/kernel/quest3/linux-build-v7`. The compiled PVT1.1 tree agrees with
the source relationships; that board selection remains a reference candidate.

| Component | Concrete dependency |
| --- | --- |
| USB storage | `USB_STORAGE`, `USB_UAS`, xHCI and DWC3 are enabled; the Qualcomm DWC3 glue and PHY modules are present in the initramfs. |
| USB role | `anorak-usb.dtsi` selects OTG and `usb-role-switch`. Its endpoint connects to the UCSI connector in `anorak.dtsi`. |
| UCSI | `ucsi_glink.c` starts UCSI setup when PMIC-GLINK reports its service up. `ucsi.c` uses the connector's role switch. |
| PMIC-GLINK | The DTS selects channel `PMIC_RTR_ADSP_APPS`, subsystem `lpass`, and protection domain `msm/adsp/charger_pd`. |
| ADSP | `qcom_q6v5_pas.c` selects `adsp.mdt` for `qcom,anorak-adsp-pas`, with `auto_boot = false`. Loading this driver alone does not start the DSP. |
| Startup | `techpack/audio/dsp/adsp-loader.c` exposes `/sys/kernel/boot_adsp/boot` and calls `rproc_boot()` when requested. Its probe initializes work without scheduling a boot. |

The reference recovery init script explicitly requests ADSP boot during its
`on init` sequence. The reference health-service script does so for charger
mode. These are evidence of the vendor startup contract, not a reason to run
Android init scripts inside Linux. Normal Android boot startup was not traced
in this check.

The firmware-free Linux initramfs contains `qcom_q6v5_pas`, `pmic_glink`,
`ucsi_glink`, the GLINK transports and `adsp_loader_dlkm`. Its firmware search
directories contain no ADSP files. An external root filesystem cannot supply
firmware needed to make that same USB root device appear. Early firmware and
the startup sequence must therefore be available in the initramfs, or another
independently verified early source. Forcing host mode would bypass the vendor
role/power path and has not been implemented.

## Existing reference firmware

`output/reference-adsp-v1/` contains data extracted with read-only `debugfs`
from the already downloaded reference vendor image. The source image SHA-256
was checked before and after extraction:

```text
e251197f09bdb2243cd15ce2c06353e660217550cd327924d1dbae2f9b8a38cc
```

The extraction preserves 51 ADSP files, totaling 13,528,343 bytes: the MDT,
split blobs and three service-description JSON files. The MDT describes 46
program headers; all 42 loadable segments with file data have the declared
split-file sizes. The MDT hash is:

```text
531f64a8f042ed722d32c1912478ce5dcbb687da73bbe962c1e7b513862d1cbd
```

The check establishes extraction identity and segment geometry. It does not
verify Qualcomm/Meta signatures, secure-world acceptance, service startup or
compatibility with the recorded headset. No firmware was executed during that extraction. The later maintained bundle
is now included in the optional startup-bearing boot containers. Extra extracted blobs are retained as reference data;
the directory is not yet a validated installation manifest.

## Startup and root lifecycle

Build with `--firmware DIRECTORY --quest-services --image
localhost/armada-vr:qcom-services`. These options are required together for this
service; firmware-only and firmware-free builds remain available. The offline
assembler adds `armada.quest=usb-root` only for a service-bearing initramfs and
checks that command line after unpacking the generated boot image.

`system/quest/armada-quest-boot` verifies the Eureka/Anorak identity and all 48
firmware hashes before loading the QRTR, ADSP loader, PMIC and UCSI modules.
`ucsi_glink` has no module alias in this vendor build, so it is loaded explicitly.
The helper finds exactly one Anorak ADSP remoteproc and waits for a real QRTR
service locator (service 64, version 1, instance 1) before requesting boot.

For an offline ADSP, an atomic marker in `/run` prevents a second boot request.
An already-running ADSP is observed without another request. The vendor loader
returns after scheduling work, so a successful write is not treated as readiness.
The service also requires ADSP `running`, the bound `PMIC_RTR_ADSP_APPS` RPMsg
channel, a bound UCSI type-C port and a DWC3 role of `host`. Each wait is capped
at 12 seconds and the whole service startup at 45 seconds. It never forces host
role or requests a DSP reset. Missing prerequisites or an unexpected mapper exit request an orderly
systemd power-off, both before and after root switch.

The first PMIC RPMsg probe populates the UCSI child, whose initial probe starts
UCSI setup. Later restarts use the PMIC notifier path. This is why a PDR callback
alone is not a sufficient first-boot readiness signal. The `uses_elf64` field in
`qcom_q6v5_pas.c` selects the coredump format; it does not require ELF64 MDT
firmware.

The service follows [systemd's root-storage daemon contract](https://systemd.io/ROOT_STORAGE_DAEMONS/):
it starts in the initramfs, survives isolation and the root-switch termination
pass, and refuses manual stop/restart. The main root needs the same unit file.
The [offline root builder](headset-root.md) now packages it alongside matching
kernel modules and firmware. The mapper continues executing from initramfs
memory; it is not replaced by a root-filesystem copy.

## Repeatable offline acceptance

```sh
python3 -B tools/test-quest-startup.py QEMU_KERNEL QEMU_STARTUP_INITRAMFS QCOM_SOURCES \
  --rootfs QEMU_RUNTIME_ROOT --output NEW_TEST_DIRECTORY
```

The runner checks all input identities and refuses physical kernel/initramfs
variants, changed source or firmware-bearing initramfs bytes, and existing
outputs. It compiles the real QMI probe from pinned sources in the same image
used to build the initramfs. Every QEMU has no network. Only the root case has a
disk, attached read-only with ext4 journal replay disabled and a RAM overlay.

The cases cover original-QEMU identity rejection, successful modeled startup,
missing firmware, a repeated request, unavailable mapper, missing host role,
actual root switch and mapper failure after root switch. The fixtures require the original QEMU identity before
modeling sysfs; they load all six real vendor modules and exchange real QRTR/QMI
messages. They do not execute DSP firmware or model USB-C electrical behavior.

Root acceptance requires the same service PID and valid QMI responses before
and after switch-root, manual restart refusal, the existing full native/Windows
VR lab checks, controlled shutdown and an unchanged backing-image checksum.
Only the service unit and test tools are staged into the RAM overlay. This is
not an installer and does not establish physical driver operation.

The initial handoff test caught systemd terminating the mapper on root switch.
`IgnoreOnIsolate` alone was insufficient; `SurviveFinalKillSignal` fixed that
lifecycle. Early fixture checks also exposed stacked `findmnt` records for the
lower ext4 and overlay mounts; the final fixture verifies the effective
filesystem with `statfs` instead. The failed runs remain in `output/quest-startup-root-v1`
through `v4`; the corrected experiment is `v5`.

September 11 acceptance is recorded in `output/quest-startup-maintained-v1/`:
all seven cases pass; the root case completes in 110.336 seconds, including
native and Windows rendering/input, and the full 8 GiB backing-image checksum
is unchanged. `output/headset-initramfs-quest-v1/` and `v2/` reproduce the same
35,418,486-byte startup initramfs. The unsigned service-bearing container
roundtrip is `output/quest3-boot-assembly-startup-v1/`. None of these results
verify physical firmware execution or headset boot acceptance.

The later `root-fault` test exposed that the original `OnFailure=emergency.target`
policy waited at an unusable main-root emergency prompt. `FailureAction=poweroff`
now requests orderly shutdown in either boot phase. The failed baseline is
preserved in `output/quest-mapper-fault-v2/`; the corrected standalone fault test
passes in `output/quest-mapper-fault-v3/` with the original backing image unchanged.
The earlier v1 fault fixture failed before injection because this vendor kernel
does not expose the optional `/proc/PID/task/PID/children` interface; the fixture
now locates the actual mapper with `pgrep` and verifies its parent.

Physical ADSP authentication, PMIC services, USB-C negotiation and storage
availability remain outside the QEMU model.

The subsequent maintained firmware preparer includes `battmgr.jsn` from the
same vendor image. It declares `msm/adsp/charger_pd`, QMI instance 74. Its 48-file
bundle includes only the MDT's required split data and metadata, plus four
service declarations. The earlier 51-file extraction remains preserved.

The UFS controller, Qualcomm PHY, SCM and crypto dependencies are also present
in the kernel/module inventory, but physical UFS operation remains unverified.
Encrypted Android userdata is still unavailable as a plain Linux root. The
initial design preserves the partition layout and calibration; no internal
storage installer or updater has been implemented.

Exact-device stock/recovery artifacts and an accepted custom-kernel boot route
remain prerequisites for physical testing. These findings do not make the
unsigned reference containers flash-ready.
