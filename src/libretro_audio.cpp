// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "core/emulator_settings.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio/audioout_backend.h"
#include "libretro_core.h"

namespace Libraries::AudioOut {
#if defined(LIBRETRO_CORE)
namespace {

class CircularAudioBuffer {
public:
    static constexpr std::size_t kCapacity = 4096;

    void Push(float left, float right) {
        std::lock_guard lock(m_mutex);
        if (m_count >= kCapacity) {
            m_read_pos = (m_read_pos + 1) % kCapacity;
            --m_count;
        }
        m_buffer[m_write_pos] = {left, right};
        m_write_pos = (m_write_pos + 1) % kCapacity;
        ++m_count;
    }

    std::size_t PopFrames(std::pair<float, float>* dest, std::size_t num_frames) {
        std::lock_guard lock(m_mutex);
        const std::size_t to_read = std::min(num_frames, m_count);
        for (std::size_t i = 0; i < to_read; ++i) {
            dest[i] = m_buffer[m_read_pos];
            m_read_pos = (m_read_pos + 1) % kCapacity;
        }
        m_count -= to_read;
        return to_read;
    }

private:
    std::mutex m_mutex;
    std::array<std::pair<float, float>, kCapacity> m_buffer{};
    std::size_t m_read_pos{0};
    std::size_t m_write_pos{0};
    std::size_t m_count{0};
};

class LibretroPortBackend;
static std::mutex s_backends_mutex;
static std::vector<LibretroPortBackend*> s_active_backends;
static LibretroPortBackend* s_master_backend{nullptr};

class LibretroPortBackend final : public PortBackend {
public:
    explicit LibretroPortBackend(const PortOut& port)
        : port_type{port.type}, frames{port.buffer_frames}, sample_rate{port.sample_rate},
          channels{port.format_info.num_channels}, is_float{port.format_info.is_float},
          frame_size{port.format_info.FrameSize()}, mix_level_padspk{port.mixLevelPadSpk} {
        std::lock_guard lock(s_backends_mutex);
        s_active_backends.push_back(this);
        if (s_master_backend == nullptr && port.type == OrbisAudioOutPort::Main) {
            s_master_backend = this;
        }
        fprintf(stderr, "[LibretroAudio] OPEN port_type=%d ch=%u rate=%u frames=%u float=%d is_master=%d\n",
                static_cast<int>(port.type), channels, sample_rate, frames, is_float ? 1 : 0,
                (s_master_backend == this) ? 1 : 0);
    }

    ~LibretroPortBackend() override {
        std::lock_guard lock(s_backends_mutex);
        std::erase(s_active_backends, this);
        if (s_master_backend == this) {
            s_master_backend = nullptr;
            for (auto* b : s_active_backends) {
                if (b->port_type == OrbisAudioOutPort::Main) {
                    s_master_backend = b;
                    break;
                }
            }
            if (s_master_backend == nullptr && !s_active_backends.empty()) {
                s_master_backend = s_active_backends.front();
            }
        }
    }

    void Output(void* ptr) override {
        if (ptr == nullptr || channels == 0 || sample_rate == 0) {
            return;
        }

        const auto* bytes = static_cast<const std::byte*>(ptr);
        const auto to_sample = [&](std::size_t frame, u32 channel) -> float {
            if (channel >= channels) {
                return 0.0f;
            }
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

        constexpr float DOWNMIX_FRONT = 1.0f;
        constexpr float DOWNMIX_CENTER = 0.7071f;
        constexpr float DOWNMIX_SURROUND = 0.7071f;

        const auto get_stereo = [&](std::size_t frame) -> std::pair<float, float> {
            float left = 0.0f;
            float right = 0.0f;

            if (channels >= 8) {
                const float fl = to_sample(frame, 0);
                const float fr = to_sample(frame, 1);
                const float fc = to_sample(frame, 2);
                const float sl = to_sample(frame, 4);
                const float sr = to_sample(frame, 5);
                const float bl = to_sample(frame, 6);
                const float br = to_sample(frame, 7);

                left = DOWNMIX_FRONT * fl + DOWNMIX_CENTER * fc + DOWNMIX_SURROUND * (sl + bl);
                right = DOWNMIX_FRONT * fr + DOWNMIX_CENTER * fc + DOWNMIX_SURROUND * (sr + br);
            } else if (channels >= 6) {
                const float fl = to_sample(frame, 0);
                const float fr = to_sample(frame, 1);
                const float fc = to_sample(frame, 2);
                const float sl = to_sample(frame, 4);
                const float sr = to_sample(frame, 5);

                left = DOWNMIX_FRONT * fl + DOWNMIX_CENTER * fc + DOWNMIX_SURROUND * sl;
                right = DOWNMIX_FRONT * fr + DOWNMIX_CENTER * fc + DOWNMIX_SURROUND * sr;
            } else if (channels == 2) {
                left = to_sample(frame, 0);
                right = to_sample(frame, 1);
            } else {
                const float mono = to_sample(frame, 0);
                left = mono;
                right = mono;
            }
            return {left, right};
        };

        bool is_master = false;
        {
            std::lock_guard lock(s_backends_mutex);
            if (s_master_backend == this || s_master_backend == nullptr) {
                is_master = true;
                s_master_backend = this;
            }
        }

        if (!is_master) {
            // Secondary port: scale and push into secondary circular buffer
            float sec_gain = gain.load(std::memory_order_relaxed);
            if (port_type == OrbisAudioOutPort::PadSpk) {
                const float pad_level =
                    static_cast<float>(mix_level_padspk.load(std::memory_order_relaxed)) / 32768.0f;
                sec_gain *= pad_level;
            }
            for (std::size_t i = 0; i < frames; ++i) {
                const auto [l, r] = get_stereo(i);
                secondary_buffer.Push(l * sec_gain, r * sec_gain);
            }
            return;
        }

        // Master port: extract primary samples, mix all active secondary buffers, soft-limit and submit
        const float master_gain = gain.load(std::memory_order_relaxed);
        std::vector<std::pair<float, float>> mixed(frames);
        for (std::size_t i = 0; i < frames; ++i) {
            const auto [l, r] = get_stereo(i);
            mixed[i] = {l * master_gain, r * master_gain};
        }

        std::vector<LibretroPortBackend*> secondaries;
        {
            std::lock_guard lock(s_backends_mutex);
            for (auto* b : s_active_backends) {
                if (b != this) {
                    secondaries.push_back(b);
                }
            }
        }

        if (!secondaries.empty()) {
            std::vector<std::pair<float, float>> sec_buf(frames);
            for (auto* sec : secondaries) {
                const std::size_t popped = sec->secondary_buffer.PopFrames(sec_buf.data(), frames);
                for (std::size_t i = 0; i < popped; ++i) {
                    mixed[i].first += sec_buf[i].first;
                    mixed[i].second += sec_buf[i].second;
                }
            }
        }

        const auto soft_limit = [](float x) -> float {
            constexpr float kThreshold = 0.8f;
            if (x > kThreshold) {
                return kThreshold + (1.0f - kThreshold) * std::tanh((x - kThreshold) / (1.0f - kThreshold));
            } else if (x < -kThreshold) {
                return -kThreshold + (1.0f - kThreshold) * std::tanh((x + kThreshold) / (1.0f - kThreshold));
            }
            return x;
        };

        std::vector<std::int16_t> converted;
        converted.reserve(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            converted.push_back(
                static_cast<std::int16_t>(std::clamp(soft_limit(mixed[i].first), -1.0f, 1.0f) * 32767.0f));
            converted.push_back(
                static_cast<std::int16_t>(std::clamp(soft_limit(mixed[i].second), -1.0f, 1.0f) * 32767.0f));
        }

        if (sample_rate == 48000) {
            Libretro::SubmitAudioSamples(converted.data(), converted.size() / 2);
        } else {
            const std::size_t output_frames = static_cast<std::size_t>(
                (static_cast<std::uint64_t>(frames) * 48000 + sample_rate / 2) / sample_rate);
            std::vector<std::int16_t> resampled;
            resampled.reserve(output_frames * 2);
            for (std::size_t i = 0; i < output_frames; ++i) {
                const auto source = std::min<std::size_t>(
                    frames - 1,
                    static_cast<std::size_t>(static_cast<std::uint64_t>(i) * sample_rate / 48000));
                resampled.push_back(converted[source * 2]);
                resampled.push_back(converted[source * 2 + 1]);
            }
            Libretro::SubmitAudioSamples(resampled.data(), resampled.size() / 2);
        }
    }

    void SetVolume(const std::array<int, 8>& channel_volumes) override {
        const float val = static_cast<float>(*std::ranges::max_element(channel_volumes)) /
                          static_cast<float>(ORBIS_AUDIO_OUT_VOLUME_0DB) *
                          EmulatorSettings.GetVolumeSlider() / 100.0f;
        gain.store(val, std::memory_order_relaxed);
    }

    void SetMixLevelPadSpk(int mix_level) override {
        mix_level_padspk.store(mix_level, std::memory_order_relaxed);
    }

    CircularAudioBuffer secondary_buffer;
    const OrbisAudioOutPort port_type;

private:
    u32 frames;
    u32 sample_rate;
    u32 channels;
    bool is_float;
    u32 frame_size;
    std::atomic<float> gain{1.0f};
    std::atomic<int> mix_level_padspk{ORBIS_AUDIO_OUT_MIXLEVEL_PADSPK_DEFAULT};
};

} // namespace

std::unique_ptr<PortBackend> LibretroAudioOut::Open(PortOut& port) {
    return std::make_unique<LibretroPortBackend>(port);
}
#endif
} // namespace Libraries::AudioOut
