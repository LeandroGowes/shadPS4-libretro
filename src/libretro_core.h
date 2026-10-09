// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#if defined(LIBRETRO_CORE)

#include <cstddef>
#include <cstdint>

namespace Libretro {

void SubmitVideoFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                      std::size_t pitch);
void SubmitAudioSamples(const std::int16_t* samples, std::size_t frames);

} // namespace Libretro

#endif
