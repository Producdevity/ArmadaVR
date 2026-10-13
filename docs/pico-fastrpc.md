# Pico sensor RPC runtime

The native ARM64 library combines Qualcomm FastRPC at the revision pinned in
`profiles/pico-fastrpc.json` with Pico's vendor ioctl ABI and ION allocator.
The upstream listener, module loading and buffer registry remain in use.
It is built as a separate bundle; the builder neither installs it into a
headset root nor starts a device service.

```sh
docker build --platform linux/arm64 -f Containerfile.fastrpc -t localhost/reframed-vr:fastrpc-tools .
tools/build-fastrpc.sh source.tar.gz output/fastrpc
```

Use the source URL and SHA-256 in the profile. An existing source archive can
be reused. The wrapper verifies it before creating the output directory and
runs the build without network access. It records the immutable compiler
image, input hashes, tests, dependency versions and bundle contents in
`bundle/build.json`. The bundle contains source, patches and licenses under
`opt/reframed-vr/fastrpc/share`; runtime libraries are under `lib64`.

The sensor process must select `createstaticpd:sensorspd` through Qualcomm's
`ITRANSPORT_PREFIX` interface before opening its sensor module. Sensor
attachment errors are fatal. Other process-creation modes are unsupported.
The adapter translates the mainline NOVA bit to the vendor bit, packs separate
FD and attribute arrays, and supports legacy static mappings. Newer delayed
FD mapping modes return unsupported instead of being reinterpreted.

For default `rpcmem` allocations, `PICO_ION_HEAP_ID` must name an independently
verified heap from 0 through 31. There is no implicit physical-device default.
The VM uses its deliberately configured system heap 25; that does not establish
the correct heap or cache policy for the headset's sensor DSP. Explicit heap
arguments remain available. Cached, uncached, default-heap and no-registration
flags are supported; other allocation flags are rejected.

Registration failures release the allocation. Deregistration or unmapping
failures retain buffer ownership, and allocator shutdown does not free buffers
that could still be used by the DSP. Consumers must finish DSP operations and
release their buffers before unloading the library. Direct CPU access to a
stream buffer still requires DMA-BUF synchronization through its descriptor.

The build runs allocator and upstream adapter tests with ASan/UBSan, checks
dynamic symbol resolution, and confirms that the runtime test refuses to run
outside its VM fixture. `tests/pico-fastrpc-runtime.c` exercises the full library
against the real kernel's virtual ION heap: allocation, buffer registration,
cache synchronization, release and failed startup with no DSP attached.

Live sensor attachment, reverse RPC from the physical DSP, stream layout,
SUID selection, calibration and tracking remain separate acceptance steps.
These tests do not establish boot/recovery or flash readiness.

`libpico-sensors.so.1` connects the low-latency stream encoder to FastRPC's
domain-aware handles, explicitly selecting SDSP and the sensor process. The
`pico-sensor-rpc.h` interface borrows a registered ION buffer; it does not choose
a heap, allocate memory, select a SUID, start a service or interpret samples.
Keep each client on one thread and keep its buffer and runtime library alive
until close succeeds. Configuration can add multiple sensors to one channel.

Close stops configured streams, deinitializes the channel and releases the RPC
handle in that order. A failed operation leaves the last confirmed state and
does not retry. A failed or malformed initialization has an uncertain channel
state, so close refuses to discard it. Its buffer must remain owned until an
independently confirmed DSP teardown; this recovery path is not yet verified.
The client never equates a failed RPC with proof that the DSP released memory.

## Offline root installation

Pass `--fastrpc /path/to/verified/bundle` to `tools/build-headset-root.py` when
building a Pico root with its DSP startup services. The installer verifies the
source and profile hashes, complete library inventory, ARM64 ELF headers,
permissions, licenses and relative library links before copying the bundle.
The libraries stay under `/opt/reframed-vr/fastrpc`; installation does not select
a heap or start a sensor stream.

The root builder checks symbol resolution using the target root's dynamic
loader and libraries, then reads the installed files back from the resulting
ext4 image. The build record binds these checks to the bundle and kernel.
These checks establish offline packaging and dependency compatibility only.
