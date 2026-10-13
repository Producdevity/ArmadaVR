# Project naming and existing builds

The public name is **Reframed VR**. Use `reframed-vr` for the repository,
checkout, package paths and container namespace; `ReframedVR` is the compact
handle. The repository is [Producdevity/reframed-vr](https://github.com/Producdevity/reframed-vr)
and its default branch is `master`.

Current builds use these identifiers:

| Surface | Name |
|---|---|
| Runtime and metadata | `/opt/reframed-vr`, `/usr/share/reframed-vr`, `/etc/reframed-vr` |
| Per-user data | `~/.local/share/reframed-vr`, `~/.local/state/reframed-vr` |
| Root filesystem label | `reframed-vr-root` (16 bytes, the ext4 limit) |
| Startup services | `reframed-quest-boot`, `reframed-pico-qrtr`, `reframed-pico-pd-mapper` |
| Kernel command-line namespace | `reframed.*` |
| SteamVR virtual driver | `reframed_virtual` |
| Test environment and markers | `REFRAMED_*` |
| Container images | `localhost/reframed-vr:*` |

Build outputs and evidence from before the rename retain their original names,
embedded paths, source hashes and checksums. They remain historical artifacts;
renaming their directories or editing their manifests does not rebuild them.
Use matching tools from their recorded Git revision to reproduce those tests.

The headset root builder rejects exports containing the previous runtime paths
before changing the extracted filesystem.

For a current runtime, rebuild the affected packages, services, root filesystem
and boot containers together into new output directories. New boot parameters,
diagnostic readers and launchers must not be mixed with old binaries. Preserve
existing Proton prefixes, VM disks, firmware, calibration and device backups;
the rename does not authorize deleting or modifying them. Documentation uses
current identifiers; dated test results refer to the recorded build revision.

Existing Docker build volumes may be kept under their original names. The
kernel wrapper accepts `KERNEL_BUILD_VOLUME` to reuse a retained cache; its
source identity checks still apply. Compiler-only images can be given a new
tag without copying layers. Runtime images containing old paths need rebuilding.

References to Armada and armada-packages that identify upstream work remain
unchanged, including the pinned FEX registry source. See the [credits](../README.md#credits).
