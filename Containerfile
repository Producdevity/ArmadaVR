ARG FEX_PKG=ghcr.io/armada-os/armada-packages/fex@sha256:277a25328499761e570bfb1f7fdce44b29dee074d76462402b0d8c361dacdffc
FROM ${FEX_PKG} AS fex
FROM registry.fedoraproject.org/fedora:44@sha256:083414712cefa4ea7c219bb850b8d8027d0c7b8978bc743780269ebbdb700eb7 AS development
RUN dnf -y install monado openxr-devel openxr-simple-playground \
    mesa-vulkan-drivers vulkan-tools vulkan-devel \
    gcc-c++ cmake ninja-build python3 patch xorg-x11-server-Xvfb \
    xorg-x11-xauth xdpyinfo ImageMagick procps-ng \
    && dnf clean all
WORKDIR /workspace
CMD ["bash"]

FROM development AS samples
RUN dnf -y --setopt=install_weak_deps=False install jsoncpp-devel glslc libX11-devel libXrandr-devel libXxf86vm-devel \
    && dnf clean all
ADD --checksum=sha256:3a6f217eda99c5535ad2626e34a67147ba63b8e6fe398183b484d7d43846642a \
    https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/b5fd54b056650ab8a148c50d0cce9ac912dd4d1e.tar.gz /tmp/openxr.tar.gz
RUN mkdir /sdk && tar -xf /tmp/openxr.tar.gz -C /sdk --strip-components=1 \
    && cmake -S /sdk -B /sdk-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_API_LAYERS=OFF -DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_WITH_SYSTEM_JSONCPP=ON \
    && cmake --build /sdk-build --target hello_xr --parallel 2 \
    && install -m755 /sdk-build/src/tests/hello_xr/hello_xr /usr/local/bin/hello_xr \
    && mkdir -p /usr/local/share/licenses/openxr-hello \
    && cp -r /sdk/LICENSE* /usr/local/share/licenses/openxr-hello/ \
    && rm -rf /sdk /sdk-build /tmp/openxr.tar.gz

FROM samples AS lab
COPY CMakeLists.txt /src/CMakeLists.txt
COPY src /src/src
RUN cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /build --parallel 2 && cmake --install /build
COPY tools/test-xr.sh /usr/local/bin/test-xr
COPY tools/test-input.py /usr/local/bin/test-input
COPY tools/render-xr.sh /usr/local/bin/render-xr
COPY tools/check-render.py /usr/local/libexec/armada-vr/check-render.py
RUN chmod +x /usr/local/bin/test-input
CMD ["test-xr", "/output"]

FROM lab AS compatibility
RUN --mount=from=fex,source=/rpms,target=/rpms \
    dnf -y --setopt=install_weak_deps=False install \
      /rpms/fex-emu-[0-9]*.rpm /rpms/fex-emu-filesystem-*.rpm clang lld \
    && dnf clean all
COPY tests/fex-smoke.S /tmp/fex-smoke.S
RUN mkdir -p /usr/local/libexec/armada-vr \
    && clang --target=x86_64-linux-gnu -fuse-ld=lld -nostdlib -static \
       /tmp/fex-smoke.S -o /usr/local/libexec/armada-vr/fex-smoke \
    && rm /tmp/fex-smoke.S
COPY tools/test-fex.sh /usr/local/bin/test-fex

FROM compatibility AS vm
RUN dnf -y --setopt=install_weak_deps=False install kernel-core kernel-modules-core dracut dracut-config-generic e2fsprogs \
    && dnf clean all \
    && mkdir /vm-boot \
    && kver=$(ls /usr/lib/modules | sort -V | tail -1) \
    && cp /usr/lib/modules/$kver/vmlinuz /vm-boot/Image \
    && dracut --force --no-hostonly --no-hostonly-cmdline --kver "$kver" \
       --add-drivers 'virtio_pci virtio_blk ext4' /vm-boot/initramfs.img
COPY system/armada-vr-lab.service /etc/systemd/system/armada-vr-lab.service
COPY tools/vm-test.sh /usr/local/bin/vm-test
COPY tools/make-rootfs.sh /usr/local/bin/make-rootfs
COPY tools/extract-kernel.py /usr/local/libexec/armada-vr/extract-kernel.py
RUN python3 /usr/local/libexec/armada-vr/extract-kernel.py /vm-boot/Image /vm-boot/Image
RUN systemctl enable armada-vr-lab.service \
    && mkdir -p /usr/share/armada-vr \
    && rpm -qa --qf '%{NAME} %{VERSION}-%{RELEASE} %{ARCH}\n' | sort > /usr/share/armada-vr/package-versions.txt
CMD ["/sbin/init"]
