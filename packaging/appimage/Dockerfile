# Build environment for the portable Ninja Gaiden II AppImage (packaging/appimage/build.sh): Ubuntu 22.04, so the program needs no glibc newer than 2.35 and runs on SteamOS,
# Ubuntu 22.04+, Debian 12, Fedora 36+ and other current systems.
# The first RUN matches the Ninja Gaiden Black and Wind Waker AppImages' environment layer for
# layer, so they share Docker's cache.
FROM ubuntu:22.04

ARG LLVM_VERSION=22
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release software-properties-common git file \
        python3 python3-pip pkg-config \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && apt-get update && apt-get install -y --no-install-recommends \
        g++-13 libstdc++-13-dev \
        libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev \
        libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl1-mesa-dev libgl-dev \
        libgbm-dev libdrm-dev libvulkan-dev libpulse-dev libpipewire-0.3-dev libasound2-dev libdbus-1-dev \
        libudev-dev \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
    && bash /tmp/llvm.sh ${LLVM_VERSION} \
    && apt-get install -y --no-install-recommends clang-${LLVM_VERSION} lld-${LLVM_VERSION} llvm-${LLVM_VERSION} \
    && for tool in clang clang++ ld.lld llvm-strip llvm-profdata; do \
           ln -sf /usr/bin/${tool}-${LLVM_VERSION} /usr/local/bin/${tool}; done \
    && python3 -m pip install --no-cache-dir "cmake>=3.25,<4" ninja patchelf \
    && rm -rf /var/lib/apt/lists/* /tmp/llvm.sh

# What the ReXGlue SDK and the game need besides: zstd (the shader cache), xcb for X11,
# nasm for FFmpeg's assembly.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libzstd-dev libx11-xcb-dev libxcb1-dev nasm xz-utils make \
    && rm -rf /var/lib/apt/lists/*

# FFmpeg for the game's movies (ASF files with WMV3/VC-1 video and WMA/WMA Pro sound): an LGPL
# shared build with only those, bundled into the AppImage. (The SDK builds its own FFmpeg for
# the game's XMA audio.)
ARG FFMPEG_VERSION=9.0.2
RUN wget -qO /tmp/ffmpeg.tar.xz https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz \
    && tar -xJf /tmp/ffmpeg.tar.xz -C /tmp \
    && cd /tmp/ffmpeg-${FFMPEG_VERSION} \
    && ./configure --prefix=/usr/local --cc=clang --enable-shared --disable-static \
         --disable-programs --disable-doc --disable-network --disable-autodetect \
         --disable-everything --disable-avdevice --disable-avfilter \
         --enable-demuxer=asf --enable-protocol=file --enable-parser=vc1 \
         --enable-decoder=wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro,wmavoice \
         --enable-swscale --enable-swresample \
    && make -j"$(nproc)" && make install \
    && mkdir -p /usr/local/share/ffmpeg \
    && cp LICENSE.md COPYING.LGPLv2.1 /usr/local/share/ffmpeg/ \
    && cd / && rm -rf /tmp/ffmpeg.tar.xz /tmp/ffmpeg-${FFMPEG_VERSION}
