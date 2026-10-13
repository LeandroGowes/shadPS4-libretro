<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 Libretro for Promus Play

This experimental Libretro core for Promus Play is based on the official shadPS4 source. The upstream revision used for the published builds is `0fe263a4760dfbfa973366890061749b4af0de97` (`main`, 0.19.1 WIP).

Emulation is developed by the shadPS4 project. This fork contains the Libretro integration and Promus Play adaptations for video, audio, controls, and frontend execution. The DLL is a build of this fork, not an official shadPS4 release.

## Build for Windows

Requirements: Git, CMake 3.24 or newer, Ninja, and LLVM-MinGW `20260922-ucrt-x86_64` (Clang 23.1.2). This is the compiler toolchain used for the published DLL. The build script applies the OpenAL patch included in this repository and enables static runtimes.

Run from PowerShell, changing the tool paths as needed:

```powershell
git clone --recursive https://github.com/LeandroGowes/shadPS4-libretro.git
cd shadPS4-libretro
./scripts/build-libretro-windows.ps1 `
  -CompilerBin 'C:\tools\llvm-mingw-20260922-ucrt-x86_64\bin' `
  -CMake 'C:\tools\cmake\bin\cmake.exe' -Ninja 'C:\tools\ninja.exe'
```

Equivalent manual CMake configuration, after initializing the submodules and applying `cmake/libretro-openal-clang.patch` to the `externals/openal-soft` submodule:

```powershell
cmake -S . -B build-libretro -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=clang.exe -DCMAKE_CXX_COMPILER=clang++.exe `
  -DENABLE_LIBRETRO=ON -DENABLE_TESTS=OFF `
  -DENABLE_DISCORD_RPC=OFF -DENABLE_UPDATER=OFF `
  -DSDL_SHARED=OFF -DSDL_STATIC=ON -DHAVE_ENDIAN_H=OFF
cmake --build build-libretro --target shadps4_libretro --parallel 4
```

By default, `LIBRETRO_STATIC_RUNTIME=ON` embeds libc++, libunwind, and winpthreads in the Windows core. Distribute only `build-libretro/shadps4_libretro.dll`. Windows system libraries and a Vulkan driver are still required. If `LIBRETRO_STATIC_RUNTIME=OFF`, also distribute `libc++.dll`, `libunwind.dll`, and `libwinpthread-1.dll` from the same LLVM-MinGW installation.

The OpenAL submodule has a local change in `alc/alc.cpp`: a transform over `EffectSlotCluster` uses a lambda to disambiguate `operator*` for Clang. The patch preserves the official submodule commit and is included in this fork.

## Libretro integration

- Implements the Libretro `retro_*` API with XRGB8888 video, 48 kHz stereo audio, and controller input.
- Exposes separate DualShock 4 left and right sticks so frontends can keep analog axes separate from the D-pad in automatic mapping mode.
- Maps RetroPad B/A/Y/X to Cross/Circle/Square/Triangle.
- Renders the emulator's Vulkan output to off-screen images and reads them back through the Libretro video callback. It does not open an SDL window or a separate emulator process.
- Accepts `eboot.bin`, ELF, ZAR, or a game directory containing `eboot.bin`.
- Stores emulator data in the frontend save directory under `shadPS4-libretro`.
- Loads system modules from the frontend BIOS directory under `shadPS4/sys_modules`.
- Each session requires a new process. Upstream threads and global state currently prevent safe reinitialization in the same process.
- Libretro initialization does not show the standalone SDL migration dialogs. Local TOML settings are converted by the official migrator when possible.
- Backing-memory allocation failures are reported to the frontend through the Libretro log callback, including the original Windows error code, requested size, and available commit memory. A new attempt requires a new process.
- Savestates, cheats, and disc swapping are not implemented in this integration.

Promus hosts the DLL in `promus_core_worker.exe`. Tests use the same host with temporary saves and do not modify the user's saves. A callback test alone does not prove that a game works; also check the image and `shadPS4-libretro/log/shad_log.txt`.

## Build for Linux

Use Git, CMake 3.24 or newer, Ninja, Clang, and the build dependencies listed in [`documents/building-linux.md`](https://github.com/shadps4-emu/shadPS4/blob/main/documents/building-linux.md):

```bash
./scripts/build-libretro-linux.sh
# Set the build parallelism:
JOBS=4 ./scripts/build-libretro-linux.sh
```

The output is `build-libretro-linux/dist/shadps4_libretro.so`. The Linux core can be distributed as a single `.so`; no `cores/lib` directory is required. SDL, OpenAL, FFmpeg, compression libraries, UI fonts, shaders, and the C++ runtime are embedded. `LIBRETRO_STATIC_RUNTIME=OFF` uses a shared C++ runtime instead. The configuration uses the dependency revisions pinned in this repository rather than replacing them with development versions installed on the system.

The Linux link resolves symbols inside the core and hides static-library symbols. This allows the upstream FFmpeg files to be embedded and prevents the core's C++ runtime from conflicting with the frontend's runtime.

The system still needs glibc and a Vulkan driver/loader. Some games also require PS4 system modules and fonts in the frontend BIOS directory. These console files are not included. The binary uses the glibc version from the build machine; to support older distributions, build on the oldest distribution you want to support. The x86-64 target requires AVX2.

Local validation used Clang 22.1.8 on Linux x86-64 with Burnout Paradise Remastered (`CUSA10866`) in the Promus Linux host, running in an isolated process. The package completed 6,000 iterations in 100 seconds with 1920x1080 video, 48 kHz audio, and controller input. Shutdown and reopening were checked in separate processes. This verifies startup and initial execution, not a complete playthrough. Savestates remain unavailable. Start a new process for each session.

### Portable build for older distributions

For distribution across Linux distributions, use the isolated Ubuntu 22.04 build:

```bash
./scripts/build-libretro-portable.sh
```

The host requires Bubblewrap (`bwrap`), curl, tar, Git, ripgrep, and binutils. The script prepares Ubuntu Base 22.04.5 and GCC 14 in `build-libretro-portable/rootfs`; it does not require sudo or change host packages. Network access is needed during the build, but not to run the resulting package. The base image is downloaded from `https://cdimage.ubuntu.com/ubuntu-base/releases/22.04/release/`; its SHA-256 is pinned in the script.

The archive is `build-libretro-portable/shadps4-libretro-linux-x86_64.tar.gz`. The core is at `build-libretro-portable/build/dist/shadps4_libretro.so`, alongside `release-metadata.json` and `shadps4_libretro.so.sha256`. The script also writes a `.sha256` checksum beside the tarball.

The Linux core is distributed as a single `.so`: UUID v4 uses `getrandom()`, and the SDL/libusb backends do not depend on libudev. The script checks ELF dependencies and fails if a runtime library outside system glibc is required or if the package requires glibc newer than 2.35. The C++ runtime is embedded.

This build supports x86-64 distributions with glibc 2.35 or newer and an x86-64-v3/AVX2 CPU, with a frontend and Vulkan already installed. Systems using musl, an older glibc, or another architecture need a different build. Libretro loading and API behavior were checked in Ubuntu 22.04. In the Promus Linux host, Burnout Paradise Remastered completed 6,000 iterations in 100 seconds with 1920x1080 video, audio, and controller input. This checks initial execution; it does not prove a complete playthrough or compatibility with every distribution.

## Manual GitHub Actions workflow

In **Actions**, select **Libretro Cores — Windows and Linux (Manual)** and click **Run workflow**. Leave `revision` empty to build the revision associated with that run, or enter a commit or tag that contains the Windows and Linux Libretro build scripts. Both jobs use the same resolved commit. When the jobs finish, download the two artifacts from the workflow run; they are retained for seven days.

The Windows job provides a ZIP containing the DLL, licenses, and metadata. The Linux job provides the portable package with the `.so`, licenses, metadata, and hashes. Depending on the `create_release` and `draft` inputs, the workflow can skip release creation, create a draft release, or publish the release immediately. It does not run games. A successful build alone does not prove game compatibility.

## Core options

The frontend receives 15 options through the Libretro `SET_VARIABLES`/`GET_VARIABLE` API, compatible with Promus and RetroArch. They are applied when a game is opened. Changing an option requires ending and restarting the session. `native` preserves the emulator configuration, including a per-game file when one exists. Other choices override that file only for the current session; the core does not write to or modify the original configuration.

The options cover PS4 Pro, developer mode, console language (PS4 ID), confirmation button, GPU memory/image reads, direct memory access, buffer copies, Vulkan device, pipeline cache, and Vulkan/shader/executable diagnostics. Applied values are recorded in the core log. Options are applied after `EmulatorSettings.Load(id)` and before game subsystems are created, using the official setters with session overrides.

Fullscreen, frontend scaling, and audio devices are not exposed here: the frontend controls presentation and Libretro uses its own audio output. FSR/HDR and standalone UI options are also omitted when the Libretro presentation path does not use them. Network features, profiles, and peripherals need dedicated integration and are not simulated by options. Diagnostic options can increase disk use and runtime overhead; advanced GPU settings can affect game compatibility.

## License and affiliation

This fork contains the Libretro integration and Promus Play adaptations. Preserve the upstream shadPS4 copyright notices and follow the licenses included with third-party components. This repository is not an official shadPS4 release and is not affiliated with the shadPS4 project, Sony Interactive Entertainment, or PlayStation.
