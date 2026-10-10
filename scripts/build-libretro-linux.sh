#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-$source_root/build-libretro-linux}"
jobs="${JOBS:-2}"

if [[ "$(uname -s)" != Linux ]]; then
    echo 'This script requires Linux.' >&2
    exit 1
fi

git -C "$source_root" submodule update --init --recursive --jobs "$jobs"
openal_root="$source_root/externals/openal-soft"
patch_path="$source_root/cmake/libretro-openal-clang.patch"
if git -C "$openal_root" apply --check "$patch_path" 2>/dev/null; then
    git -C "$openal_root" apply "$patch_path"
else
    git -C "$openal_root" apply --reverse --check "$patch_path"
fi

cmake -S "$source_root" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="${CC:-clang}" -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
    -DENABLE_LIBRETRO=ON -DLIBRETRO_STATIC_RUNTIME=ON \
    -DENABLE_SYSTEM_LIBRARIES=OFF -DENABLE_TESTS=OFF \
    -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON \
    -DSDL_LIBUDEV=OFF -DLIBUSB_ENABLE_UDEV=OFF "$@"
cmake --build "$build_dir" --target shadps4_libretro --parallel "$jobs"
cmake -DCORE="$build_dir/shadps4_libretro.so" \
    -DOUTPUT_DIR="$build_dir/dist" -P "$source_root/scripts/package-libretro-linux.cmake"
printf '%s\n' "$build_dir/dist/shadps4_libretro.so"
ldd "$build_dir/dist/shadps4_libretro.so"
