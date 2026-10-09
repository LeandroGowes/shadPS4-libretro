// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "libretro_core.h"

namespace Libraries::AudioOut {
#if defined(LIBRETRO_CORE)
namespace {

class LibretroPortBackend final : public PortBackend {
public:
    explicit LibretroPortBackend(const PortOut& port)
        : frames{port.buffer_frames}, sample_rate{port.sample_rate},
          channels{port.format_info.num_channels}, is_float{port.format_info.is_float},
          frame_size{port.format_info.FrameSize()} {}

    void Output(void* ptr) override {
        if (ptr == nullptr || channels == 0 || sample_rate == 0) {
            return;
        }

        const auto* bytes = static_cast<const std::byte*>(ptr);
        std::vector<std::int16_t> converted;
        converted.reserve(static_cast<std::size_t>(frames) * 2);
        const auto to_sample = [&](std::size_t frame, u32 channel) -> float {
            channel = std::min<u32>(channel, channels - 1);
            const std::size_t offset =
                frame * frame_size + channel * (is_float ? sizeof(float) : sizeof(std::int16_t));
            if (is_float) {
                float value;
                std::memcpy(&value, bytes + offset, sizeof(value));
                return std::clamp(value, -1.0f, 1.0f);
            }
            std::int16_t value;
            std::memcpy(&value, bytes + offset, sizeof(value));
            return static_cast<float>(value) / 32768.0f;
        };
        const auto append_frame = [&](std::size_t frame) {
            const float left = to_sample(frame, 0) * gain;
            const float right = to_sample(frame, channels == 1 ? 0 : 1) * gain;
            converted.push_back(
                static_cast<std::int16_t>(std::clamp(left, -1.0f, 1.0f) * 32767.0f));
            converted.push_back(
                static_cast<std::int16_t>(std::clamp(right, -1.0f, 1.0f) * 32767.0f));
        };

        if (sample_rate == 48000) {
            for (std::size_t i = 0; i < frames; ++i)
                append_frame(i);
        } else {
            const std::size_t output_frames = static_cast<std::size_t>(
                (static_cast<std::uint64_t>(frames) * 48000 + sample_rate / 2) / sample_rate);
            for (std::size_t i = 0; i < output_frames; ++i) {
                const auto source = std::min<std::size_t>(
                    frames - 1,
                    static_cast<std::size_t>(static_cast<std::uint64_t>(i) * sample_rate / 48000));
                append_frame(source);
            }
        }
        Libretro::SubmitAudioSamples(converted.data(), converted.size() / 2);
    }

    void SetVolume(const std::array<int, 8>& channel_volumes) override {
        gain = static_cast<float>(*std::ranges::max_element(channel_volumes)) /
               static_cast<float>(ORBIS_AUDIO_OUT_VOLUME_0DB) * EmulatorSettings.GetVolumeSlider() /
               100.0f;
    }

private:
    u32 frames;
    u32 sample_rate;
    u32 channels;
    bool is_float;
    u32 frame_size;
    float gain{1.0f};
};

} // namespace

std::unique_ptr<PortBackend> LibretroAudioOut::Open(PortOut& port) {
    return std::make_unique<LibretroPortBackend>(port);
}
#endif
} // namespace Libraries::AudioOut
