# Quest early storage and ADSP dependencies

The Linux boot containers and volatile-root handoff are verified offline, but
the current initramfs cannot yet establish a working Quest USB root path. It
includes the compiled USB and remote-processor drivers without the ADSP firmware
or an explicit ADSP startup step. This is a remaining boot integration task.

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

The inspected Linux initramfs contains `qcom_q6v5_pas`, `pmic_glink`,
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
compatibility with the recorded headset. No firmware was executed or added to
the boot containers. Extra extracted blobs are retained as reference data;
the directory is not yet a validated installation manifest.

## Next boot integration

1. Define the exact MDT, required split segments and service-discovery data for
   the selected firmware, retaining their source-image identity. Validate the
   manifest before adding firmware to an initramfs.
2. Add a bounded Linux early-startup sequence that waits for the actual vendor
   ADSP loader, starts it once, and verifies the required PMIC-GLINK/UCSI state
   before waiting for USB root. Trace service-discovery dependencies as part of
   this work; the JSON files alone do not establish a running service registry.
3. Test ordering, missing firmware, missing services and controlled shutdown
   offline. QEMU cannot validate DSP authentication, USB-C power negotiation or
   physical storage enumeration.

The UFS controller, Qualcomm PHY, SCM and crypto dependencies are also present
in the kernel/module inventory, but physical UFS operation remains unverified.
Encrypted Android userdata is still unavailable as a plain Linux root. The
initial design preserves the partition layout and calibration; no internal
storage installer or updater has been implemented.

Exact-device stock/recovery artifacts and an accepted custom-kernel boot route
remain prerequisites for physical testing. These findings do not make the
unsigned reference containers flash-ready.
