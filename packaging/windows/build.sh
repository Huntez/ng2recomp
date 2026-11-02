#!/usr/bin/env bash
# Ninja Gaiden II for Windows x64, cross-compiled on Linux.
#
#   packaging/windows/build.sh
#
# Builds the ReXGlue SDK for Windows (thirdparty/rexglue-sdk/out/install/windows-x64) when it
# isn't there, fetches FFmpeg's Windows build into out/windows-deps once, builds the game
# (cmake/toolchains/windows-x64-clang-xwin.cmake: clang and lld against Microsoft's C runtime
# and Windows SDK from xwin in ~/.xwin) and packs out/windows/NinjaGaiden2/ -- the program and
# the DLLs it needs that Windows doesn't ship -- into out/windows/NinjaGaiden2-windows-x64.zip.
# No game data: the first launch asks for the game's folder and Title Update 2.
#
# Needs: a native build first (out/build/linux-amd64-release: its code generator output and
# shader cache are used), cmake, ninja, clang, lld, curl, bsdtar, and xwin's splat
# (`xwin --accept-license splat --output ~/.xwin`: the license is yours to accept).
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
toolchain=$repo/cmake/toolchains/windows-x64-clang-xwin.cmake
sdk=$repo/thirdparty/rexglue-sdk
deps=$repo/out/windows-deps
out=$repo/out/windows
ffmpeg=ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0
ffmpeg_url=https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/$ffmpeg.zip
ffmpeg_sha256=7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646
mkdir -p "$deps" "$out"

[ -f "$repo/out/build/linux-amd64-release/shaders/ng2_shader_cache.cpp" ] ||
    { echo "No native build: build out/build/linux-amd64-release first (README)." >&2; exit 1; }

# The SDK for Windows (its D3D12 backend off: the game draws with its own Vulkan renderer),
# brought up to date every time (cheap when nothing changed).
if [ ! -f "$sdk/out/build/windows-x64/build.ninja" ]; then
    cmake -S "$sdk" -B "$sdk/out/build/windows-x64" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=23 \
        -DCMAKE_INSTALL_PREFIX="$sdk/out/install/windows-x64" \
        -DREXGLUE_USE_D3D12=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_ENABLE_TRACY=OFF \
        >"$out/sdk-configure.log"
fi
cmake --build "$sdk/out/build/windows-x64" -j"${JOBS:-$(nproc)}" >"$out/sdk-build.log"
cmake --install "$sdk/out/build/windows-x64" >/dev/null

# FFmpeg (LGPL shared build: headers, import libraries, DLLs)
if [ ! -f "$deps/ffmpeg/lib/avformat.lib" ]; then
    [ -s "$deps/$ffmpeg.zip" ] || curl -fL -o "$deps/$ffmpeg.zip" "$ffmpeg_url" ||
        { echo "FFmpeg's build is gone from $ffmpeg_url: put $ffmpeg.zip in $deps." >&2; exit 1; }
    echo "$ffmpeg_sha256  $deps/$ffmpeg.zip" | sha256sum -c --quiet
    rm -rf "$deps/ffmpeg" && mkdir -p "$deps/ffmpeg"
    bsdtar -xf "$deps/$ffmpeg.zip" -C "$deps/ffmpeg" --strip-components 1
fi

build=$repo/out/build/windows-x64
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$sdk/out/install/windows-x64" \
    -DNG2_FFMPEG_DIR="$deps/ffmpeg" >"$out/configure.log"
cmake --build "$build" -j"${JOBS:-$(nproc)}" --target ng2 >"$out/build.log"

pkg=$out/NinjaGaiden2
rm -rf "${pkg:?}"
mkdir -p "$pkg/licenses"
llvm-strip -o "$pkg/ng2.exe" "$build/ng2.exe"
cp "$sdk/out/install/windows-x64/bin/"{rexruntime,rexgpu-xenos,SDL3}.dll "$pkg/"
cp "$deps/ffmpeg/bin/"{avformat-63,avcodec-63,avutil-61,swresample-7,swscale-10}.dll "$pkg/"
cp "$repo/LICENSE" "$repo/THIRD_PARTY.md" "$pkg/licenses/"
cp "$deps/ffmpeg/LICENSE.txt" "$pkg/licenses/LICENSE-FFmpeg.txt"
cat > "$pkg/README.txt" <<'TXT'
Ninja Gaiden II - static recompilation for Windows (Vulkan)

Run ng2.exe. The first time it asks for the folder with your copy of the game (the one with
default.xex, extracted from your disc) and for Title Update 2 (the package
TU_1A461UL_000000G000000.00000000000G4, or its default.xexp), which it copies into that folder.
Downloadable content: put the packages in a "dlc" folder inside the game folder.

Needs a GPU with Vulkan 1.3 drivers and the Microsoft Visual C++ 2015-2022 Redistributable
(x64): https://aka.ms/vs/17/release/vc_redist.x64.exe

Settings, saves and the log are in %LOCALAPPDATA%\NinjaGaiden2. No game data is included.
TXT
rm -f "$out/NinjaGaiden2-windows-x64.zip"
(cd "$out" && bsdtar -a -cf NinjaGaiden2-windows-x64.zip NinjaGaiden2)
echo "$out/NinjaGaiden2-windows-x64.zip"
