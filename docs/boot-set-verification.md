# Quest boot-set verification

Quest boot packaging and recoverable boot are the current bring-up priority.
The read-only verifier checks a local reference set before it can be used to
develop boot-image assembly. It never generates an image, contacts a headset,
changes partitions, disables AVB or changes rollback values.

## Authenticate the OTA first

Obtain `otacerts.zip` independently from the stock device or another already
authenticated source. The certificate embedded in an untrusted OTA cannot
establish its own trust. Keep the original firmware and certificate copies
outside Git.

```sh
python3 -B tools/verify-ota.py PATH_TO_STOCK_OTA.zip \
  --device-certificates PATH_TO_DEVICE_OTACERTS.zip \
  --expected-device eureka --expected-build EXPECTED_INCREMENTAL
```

The equivalent recipe is `just verify-ota OTA CERTIFICATES DEVICE BUILD`.
The verifier reads local regular files only. It authenticates the whole ZIP
with OpenSSL using the supplied certificates, excluding embedded package
certificates from signer lookup. It then checks the authenticated A/B target
metadata and the payload's full SHA-256, metadata SHA-256 and header geometry.
It does not contact a device, unpack partitions, modify inputs or install an
update. Python 3.11+ and OpenSSL with the `cms` command are required on a POSIX
host.

Supported packages use a single detached PKCS7 SHA-256/RSA signature without
CMS attributes, non-ZIP64 directory geometry, and a stored version-2 payload.
Unsupported formats fail closed. The signed byte range and ambiguous-end-record
checks follow the [AOSP recovery verifier](https://android.googlesource.com/platform/bootable/recovery/+/ceb7924c43dc2e1be597ffa7b7f9fd5c5f7ddcb7/otautil/verifier.cpp).
Certificate lifetime and PKI chain validation are not used; the caller must
provide independently trusted device signing keys, as Android recovery does.

A successful result establishes authentication under those supplied keys. It
does not establish recovery-key provisioning, stored rollback values, update
acceptance, recoverability after boot-chain changes, or custom-kernel boot.
Those proof fields remain false. Use the separate boot-set verifier below on
partition images reconstructed from that authenticated package, preserving
the package, extraction and image hashes as one provenance chain.

## Verify extracted boot images

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

## Exact-build result, October 7, 2026

The local `52083180032000520` OTA authenticates under the release certificate
copied read-only from stock Quest 3. Its archive SHA-256 is
`0e7c4b8b7668741f6538a6a4a598011b39950f0a6d24dbfba48be91889e40719`.
The payload and metadata hashes pass. Extracted `boot`, `vendor_boot`, `dtbo`,
`vbmeta` and `vbmeta_system` pass boot-set integrity verification. Both signed
VBMeta images use SHA256_RSA2048, flags zero, and rollback index `1767571200`
at effective locations 0 and 1. The boot/vendor_boot containers use header
version 4. The reported DTBO index 8 maps to Eureka PVT1.1 in this package's
13-entry table; that index must not be applied to a different build's table.

This is an authenticated stock input, not a tested restore. The OTA does not
contain a separate recovery partition payload or unit-specific calibration.
Its images are not backups of both installed slots. Bootloader acceptance,
stored rollback values, runtime memory-map adjustments and restoration after
a modified boot chain remain unverified.

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
