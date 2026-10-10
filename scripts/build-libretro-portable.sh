#!/usr/bin/env bash
set -euo pipefail

source_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
portable_root="$source_root/build-libretro-portable"
rootfs="$portable_root/rootfs"
base_url=https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release
base_name=ubuntu-base-22.04.5-base-amd64.tar.gz
mkdir -p "$rootfs"
if [[ ! -e "$rootfs/etc/os-release" ]]; then
    curl -fL "$base_url/$base_name" -o "$portable_root/$base_name"
    (cd "$portable_root" && printf '%s  %s\n' \
        242cd8898b33ea806ef5f13b1076ed7c76f9f989d18384452f7166692438ff1a \
        "$base_name" | sha256sum -c -)
    tar --no-same-owner -xzf "$portable_root/$base_name" -C "$rootfs"
fi

run_base() {
    bwrap --unshare-user --uid 0 --gid 0 --unshare-pid --die-with-parent \
        --bind "$rootfs" / --proc /proc --dev /dev --tmpfs /tmp \
        --ro-bind /etc/resolv.conf /etc/resolv.conf \
        --bind "$source_root" /source --bind "$source_root" "$source_root" --chdir /source \
        --setenv HOME /root --setenv PATH /opt/cmake/bin:/usr/local/bin:/usr/bin:/bin \
        "$@"
}

if [[ ! -e "$rootfs/opt/shadps4-build-ready" ]]; then
    curl -fsSL 'https://keyserver.ubuntu.com/pks/lookup?op=get&search=0xC8EC952E2A0E1FBDC5090F6A2C277A0A352154E5' \
        -o "$portable_root/toolchain.asc"
    run_base bash scripts/setup-libretro-portable.sh
fi
run_base env CC=gcc-14 CXX=g++-14 BUILD_DIR=/source/build-libretro-portable/build \
    JOBS="${JOBS:-4}" bash scripts/build-libretro-linux.sh
while IFS= read -r -d '' library; do
    required=$(objdump -T "$library" | rg -o 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
    printf '%s: %s\n' "$library" "$required"
    if [[ "$(printf '%s\n' GLIBC_2.35 "$required" | sort -V | tail -1)" != GLIBC_2.35 ]]; then
        echo 'Package exceeds the glibc 2.35 baseline.' >&2
        exit 1
    fi
done < <(find "$portable_root/build/dist" -type f -name '*.so*' -print0)
if readelf -d "$portable_root/build/dist/shadps4_libretro.so" | rg -q 'libudev|libuuid|RPATH|RUNPATH'; then
    echo 'The Libretro core is not self-contained or still has an RPATH.' >&2
    exit 1
fi
dist="$portable_root/build/dist"
core=shadps4_libretro.so
core_sha256=$(sha256sum "$dist/$core" | cut -d' ' -f1)
commit=$(git -C "$source_root" rev-parse HEAD)
built_at=$(date -u +'%Y-%m-%dT%H:%M:%SZ')
cat > "$dist/release-metadata.json" <<EOF
{
  "repository": "LeandroGowes/shadPS4-libretro",
  "commit": "$commit",
  "platform": "linux",
  "architecture": "x86_64",
  "artifact": "$core",
  "sha256": "$core_sha256",
  "minimum_glibc": "2.35",
  "requires_system_vulkan_driver": true,
  "license_files": ["LICENSE", "LICENSES/"],
  "built_at_utc": "$built_at"
}
EOF
printf '%s  %s\n' "$core_sha256" "$core" > "$dist/$core.sha256"
archive="$portable_root/shadps4-libretro-linux-x86_64.tar.gz"
tar -czf "$archive" -C "$dist" .
printf '%s  %s\n' "$(sha256sum "$archive" | cut -d' ' -f1)" "$(basename "$archive")" \
    > "$archive.sha256"
