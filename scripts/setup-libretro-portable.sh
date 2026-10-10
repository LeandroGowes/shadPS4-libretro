#!/usr/bin/env bash
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get -o APT::Sandbox::User=root update
apt-get -o APT::Sandbox::User=root install -y --no-install-recommends \
    ca-certificates curl gnupg git ninja-build pkg-config xz-utils \
    build-essential libasound2-dev libpulse-dev libudev-dev libevdev-dev \
    libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxss-dev libxtst-dev \
    libwayland-dev libxkbcommon-dev libvulkan-dev libuuid1 uuid-dev tzdata
gpg --batch --yes --dearmor -o /usr/share/keyrings/shadps4-toolchain.gpg \
    /source/build-libretro-portable/toolchain.asc
printf '%s\n' 'deb [signed-by=/usr/share/keyrings/shadps4-toolchain.gpg] https://ppa.launchpadcontent.net/ubuntu-toolchain-r/test/ubuntu jammy main' \
    > /etc/apt/sources.list.d/shadps4-toolchain.list
apt-get -o APT::Sandbox::User=root update
apt-get -o APT::Sandbox::User=root install -y --no-install-recommends gcc-14 g++-14
curl -fL https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-linux-x86_64.tar.gz \
    -o /tmp/cmake.tar.gz
mkdir -p /opt/cmake
tar -xzf /tmp/cmake.tar.gz --strip-components=1 -C /opt/cmake
apt-get clean
touch /opt/shadps4-build-ready
