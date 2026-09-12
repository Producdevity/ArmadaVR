# Quest DMA-buffer synchronization

Mesa's display WSI imports rendering-completion sync files into DMA buffers
before presenting them. The Quest 5.10.246 vendor source lacks the standard
`DMA_BUF_IOCTL_IMPORT_SYNC_FILE` and `DMA_BUF_IOCTL_EXPORT_SYNC_FILE` ioctls.
Its SDE driver imports DMA buffers and waits on their exclusive reservation
fences in `msm_atomic_prepare_fb()` and `complete_commit()`.

`patches/kernel/linux-userspace/0002-dma-buf-sync-file.patch` backports the
[Linux 6.0 ioctl ABI](https://github.com/torvalds/linux/blob/v6.0/include/uapi/linux/dma-buf.h)
and [ioctl handling](https://github.com/torvalds/linux/blob/v6.0/drivers/dma-buf/dma-buf.c)
to the vendor reservation API. The existing kernel builder includes it in both
Linux userspace configurations; the unmodified vendor configuration is separate.

Read exports wait on writers; write exports wait on readers and writers.
Imported writers preserve earlier independent writers, while readers remain
separate shared dependencies. The patch uses the vendor tree's existing fence
unwrapping implementation to keep merged fences flat. The ordinary driver
submission API, `dma_resv_add_excl_fence()`, retains its existing behavior.

The older core assumed shared fences already depended on the exclusive writer.
Independent imports require correcting that assumption in DMA-buffer polling,
`dma_resv_test_signaled_rcu()` and `dma_resv_wait_timeout_rcu()`. A completed
reader must not hide a pending writer, and completing a writer must not skip a
pending reader. Validation and allocation failures leave the reservation
unchanged; failed export copy-back releases its descriptor.

## Diskless tests

```sh
python3 -B tools/test-dma-buf-sync.py QEMU_KERNEL --output NEW_TEST_DIRECTORY
```

The runner requires a checksummed QEMU transport kernel with built-in software
fences, debugfs and udmabuf. It compiles the fixture in the kernel build's
immutable ARM64 compiler image, then boots QEMU without disks, networking or
hardware passthrough. The fixture additionally checks the QEMU device tree and
an explicit test command-line flag before exercising the API.

The suite covers ABI and descriptor flags, empty reservations, invalid requests,
export copy-back failures, independent writers, separate read/write dependencies,
combined polling, immutable snapshots, cross-process descriptor transfer, 1,024
imports, descriptor exhaustion and recovery. Every normal case requires clean
power-off and rejects test failures or kernel warnings in the boot log.

`--expect-unsupported` verifies that an unpatched baseline rejects both ioctls.
`--reservation-module` additionally loads `dma_buf_resv_test.ko` when its hash is
recorded in that kernel's build artifacts. Its source is in
`tests/kernel-dma-buf/`; it exercises the real reservation query and wait helpers
with kernel software fences, including a writer signaled during a wait while a
reader stays pending. The module refuses non-QEMU device trees.

## Recorded results and limits

`output/dma-buf-sync-baseline-v1/` confirms both ioctls are absent in
`output/kernel/quest3/qemu-abi-v7/`. The first backport passed its initial ioctl
suite, but the added polling regression reproduced premature readiness in
`output/dma-buf-sync-poll-regression-v1/`. Those artifacts and the earlier patch
are preserved.

`output/dma-buf-sync-v2/` passes the corrected ioctl suite, polling regression and
both reservation tests using `output/kernel/quest3/qemu-dmabuf-v3/`. It performs
1,024 imports without file-descriptor growth and powers off cleanly. These are
real kernel DMA-buffer/software-fence results, with no GPU or panel execution.
The physical configuration also compiles as
`output/kernel/quest3/physical-dmabuf-v3/` with the same corrected source patch;
that kernel has not run on a headset.

The incremental build uses private copy-on-write source/build layers over the
existing kernel cache mounted read-only. Build records contain the baseline,
patch, complete source-tree identity, compiler commands and output hashes.
Those incremental test directories retain their recorded overlay-cache
requirement. Complete exports are now available at
`output/kernel/quest3/linux-build-v10/` and `qemu-abi-v10/`; packaging their
Image, 272 modules and 14 device trees no longer requires access to the cache.
Compiling additional external test modules still requires the original matching
source/build layers; older runners that infer a plain cache path from
`olddefconfig` commands do not consume these export records.

The exports read both original and incremental caches through read-only mounts.
Module installation and stripping run in RAM, with checksummed output copied to
the host only after verifying its size and remaining headroom. Source identity,
Image, configuration and cached module hashes are checked around the export.
The full new SDE module differs from the older bundle; the other 271 module
hashes and all device trees match. Export logs and provenance remain in each
build directory. The v8 missing-Python-path and v9 insufficient-RAM failures are
preserved; v10 completes successfully.

Matching startup artifacts are `output/headset-initramfs-quest-v4/` and
`headset-initramfs-quest-qemu-v6/`. The new QEMU package passes actual module load
and QRTR/QMI readiness in `output/quest-startup-dmabuf-v1/`, with modeled peripheral
state and clean power-off. This does not repeat or replace the earlier DMA-buffer
ioctl tests, whose kernel Image is identical. The existing v3 full roots remain
unchanged and still need the new kernels/modules and graphics/runtime bundles.

This API closes one synchronization prerequisite. KGSL execution, SDE buffer
import and scanout, physical panel timing, tracking, thermal behavior and the
exact-device boot/recovery gates remain unverified. These results do not
establish installation or flash readiness.
