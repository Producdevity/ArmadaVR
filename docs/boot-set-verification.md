# Quest boot-set verification

Quest boot packaging and recoverable boot are the current bring-up priority.
The read-only verifier checks a local reference set before it can be used to
develop boot-image assembly. It never generates an image, contacts a headset,
changes partitions, disables AVB or changes rollback values.

Fetch the pinned AOSP tool once, then work offline:

```sh
python3 -B tools/fetch-avb.py output/avb
python3 -B tests/boot-set-tests.py output/avb/avbtool.py
python3 -B tools/verify-boot-set.py PATH_TO_LOCAL_IMAGES \
  --expected-build EXPECTED_INCREMENTAL --avbtool output/avb/avbtool.py
```

Python 3.11 or later and OpenSSL are required. The input directory must contain
`metadata`, `boot.img`, `vendor_boot.img`, `dtbo.img`, `vbmeta.img` and every
chained VBMeta image. Inputs must be regular files, not symlinks or device nodes.
The supported scope is raw Eureka A/B images, ordinary salted SHA-256/SHA-512
boot hashes and AVB metadata with verification enabled. Unsupported descriptor
types, flags and metadata geometry are rejected.

The tool checks:

- The OTA metadata declares Eureka and the requested incremental. This metadata
  is not authenticated by this check; changing a text field cannot establish
  firmware identity.
- AVB footer, authentication, auxiliary and descriptor bounds are consistent.
- The three boot payloads match their own footer hashes and the hashes in signed
  metadata. Regenerating an unsigned footer cannot hide a changed boot payload.
- Each VBMeta signature matches its embedded key. Chained images use the key
  specified by their parent, and rollback index locations do not conflict.
- Full input checksums and the partitions outside this verification scope are
  recorded. System/vendor/ODM filesystem hash trees are explicitly unchecked.

The AOSP implementation is pinned by commit, size and SHA-256 in
`profiles/avb.json`; modified copies are rejected before execution. Parsing and
signature verification use that upstream implementation. The local wrapper
adds bounded file access, boot-set coverage and explicit proof limits. See
[AOSP AVB](https://android.googlesource.com/platform/external/avb/+/ba2dec4b035b0a3b61c5f8f8a74d86bcd450b1ee/README.md)
for the distinction between signature validation, platform key trust and stored
rollback values. Android container geometry is a separate check provided by
`tools/inspect-boot.py`.

## Reference result, September 11, 2026

The existing `52433670036000520` reference passes boot-set integrity checks.
Its `boot`, `vendor_boot` and `dtbo` footer metadata uses algorithm `NONE`; their
payload hashes are covered by `vbmeta`, signed with SHA256_RSA2048. The
`vbmeta_system` signature and parent-key relationship also pass. Both signed
images contain rollback index `1780444800`; their effective index locations
are 0 and 1. Verification flags are zero. Seven filesystem partitions remain
outside the check.

The recorded headset incremental is `52083180031500520`. Supplying that build
as the expected value rejects the available reference. Evidence is retained in
`output/quest3-boot-set-v1/`; the original images remain unchanged.

The successful reference result does **not** establish manufacturer-key trust,
authenticated OTA provenance, acceptance of these rollback values by the unit,
unsigned-kernel boot, board compatibility or successful recovery. These remain
explicitly false in the report. No zero-brick-risk or flash-ready claim follows.

## Order of the remaining boot work

1. Obtain an authenticated stock boot/recovery set matching the headset and
   establish what boot method its actual lock state permits. The newer reference
   and a temporary Android root shell cannot substitute for that evidence.
2. Implement and test the Quest boot/vendor_boot/DTBO assembly and Linux
   initramfs handoff against the verified container contract. Preserve the stock
   bootloader, partition layout and calibration. Do not assume that a rootfs
   file in encrypted Android userdata is accessible during early boot.
3. Exercise the proposed nonpersistent boot and independent recovery route on
   the exact unit only after separate physical-operation authorization. This
   must precede any persistent installer or updater design that depends on them.
4. Complete hardware GPU/display, tracking, controllers, audio and thermal
   acceptance before calling the first standalone VR version working. VM input,
   Windows rendering and reproducible-image work remain required alongside it.

Local commits of finished, verified Armada VR work are authorized. Device writes
and pushes are not authorized. Historical evidence and unfinished work are kept.
