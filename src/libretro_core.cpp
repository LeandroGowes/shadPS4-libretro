// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "libretro.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include <common/key_manager.h>
#include <common/path_util.h>
#include <common/singleton.h>
#include <core/emulator_state.h>
#include <core/ipc/ipc.h>
#include <core/libraries/kernel/time.h>
#include <input/controller.h>
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "emulator.h"

namespace {

retro_environment_t environment_callback{};
retro_video_refresh_t video_callback{};
retro_audio_sample_t audio_callback{};
retro_audio_sample_batch_t audio_batch_callback{};
retro_input_poll_t input_poll_callback{};
retro_input_state_t input_state_callback{};

struct VideoFrame {
    std::vector<std::uint32_t> pixels;
    unsigned width{};
    unsigned height{};
};

std::mutex queue_mutex;
std::deque<VideoFrame> video_queue;
std::deque<std::int16_t> audio_queue;
std::filesystem::path loaded_content;
bool game_loaded{};
// The upstream emulator owns process-wide state and cannot yet stop its guest
// pthreads. Do not let a frontend reuse this core instance for another game.
bool session_started{};
std::atomic_bool frontend_active{true};
constexpr std::size_t kMaxQueuedAudioFrames = 48000 * 2;

void KeepCoreLoadedUntilProcessExit() {
#if defined(_WIN32)
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&retro_api_version), &module);
#elif defined(__linux__)
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&retro_api_version), &info) != 0 && info.dli_fname) {
#ifdef RTLD_NODELETE
        dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
#else
        dlopen(info.dli_fname, RTLD_NOW);
#endif
    }
#endif
}

void SetUserDirectory(const std::filesystem::path& root) {
    using Common::FS::PathType;
    static constexpr std::array<std::pair<PathType, std::string_view>, 20> paths{{
        {PathType::UserDir, ""},
        {PathType::LogDir, "log"},
        {PathType::ScreenshotsDir, "screenshots"},
        {PathType::ShaderDir, "shader"},
        {PathType::GameDataDir, "data"},
        {PathType::TempDataDir, "temp"},
        {PathType::SysModuleDir, "sys_modules"},
        {PathType::DownloadDir, "download"},
        {PathType::CapturesDir, "captures"},
        {PathType::CheatsDir, "cheats"},
        {PathType::PatchesDir, "patches"},
        {PathType::MetaDataDir, "game_data"},
        {PathType::CustomTrophy, "custom_trophy"},
        {PathType::CustomConfigs, "custom_configs"},
        {PathType::CacheDir, "cache"},
        {PathType::FontsDir, "fonts"},
        {PathType::TrophyDir, "trophy"},
        {PathType::HomeDir, "home"},
        {PathType::CustomModulesDir, "custom_modules"},
        {PathType::LicensesDir, "licenses"},
    }};
    for (const auto& [type, child] : paths) {
        const auto path = child.empty() ? root : root / child;
        std::filesystem::create_directories(path);
        Common::FS::SetUserPath(type, path);
    }
}

std::filesystem::path GetEnvironmentDirectory(unsigned command) {
    const char* directory{};
    if (!environment_callback || !environment_callback(command, &directory) || !directory ||
        *directory == '\0') {
        return {};
    }
    return std::filesystem::u8path(directory);
}

void PollInput() {
    if (!input_poll_callback || !input_state_callback) {
        return;
    }
    input_poll_callback();

    using Button = Libraries::Pad::OrbisPadButtonDataOffset;
    Input::State state{};
    const u64 timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    const auto pressed = [](unsigned id) {
        return input_state_callback(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    };
    state.OnButton(Button::Cross, pressed(RETRO_DEVICE_ID_JOYPAD_B));
    state.OnButton(Button::Circle, pressed(RETRO_DEVICE_ID_JOYPAD_A));
    state.OnButton(Button::Square, pressed(RETRO_DEVICE_ID_JOYPAD_Y));
    state.OnButton(Button::Triangle, pressed(RETRO_DEVICE_ID_JOYPAD_X));
    state.OnButton(Button::TouchPad, pressed(RETRO_DEVICE_ID_JOYPAD_SELECT));
    state.OnButton(Button::Options, pressed(RETRO_DEVICE_ID_JOYPAD_START));
    state.OnButton(Button::Up, pressed(RETRO_DEVICE_ID_JOYPAD_UP));
    state.OnButton(Button::Down, pressed(RETRO_DEVICE_ID_JOYPAD_DOWN));
    state.OnButton(Button::Left, pressed(RETRO_DEVICE_ID_JOYPAD_LEFT));
    state.OnButton(Button::Right, pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT));
    state.OnButton(Button::L1, pressed(RETRO_DEVICE_ID_JOYPAD_L));
    state.OnButton(Button::R1, pressed(RETRO_DEVICE_ID_JOYPAD_R));
    state.OnButton(Button::L2, pressed(RETRO_DEVICE_ID_JOYPAD_L2));
    state.OnButton(Button::R2, pressed(RETRO_DEVICE_ID_JOYPAD_R2));
    state.OnButton(Button::L3, pressed(RETRO_DEVICE_ID_JOYPAD_L3));
    state.OnButton(Button::R3, pressed(RETRO_DEVICE_ID_JOYPAD_R3));

    const auto axis = [](unsigned index, unsigned axis_id) {
        if (!input_state_callback)
            return 128;
        const int raw = input_state_callback(0, RETRO_DEVICE_ANALOG, index, axis_id);
        return (raw + 32768) / 256;
    };
    state.OnAxis(Input::Axis::LeftX, axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X),
                 timestamp, false);
    state.OnAxis(Input::Axis::LeftY, axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y),
                 timestamp, false);
    state.OnAxis(Input::Axis::RightX,
                 axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X), timestamp, false);
    state.OnAxis(Input::Axis::RightY,
                 axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y), timestamp, false);
    state.OnAxis(Input::Axis::TriggerLeft, pressed(RETRO_DEVICE_ID_JOYPAD_L2) ? 255 : 0, timestamp,
                 false);
    state.OnAxis(Input::Axis::TriggerRight, pressed(RETRO_DEVICE_ID_JOYPAD_R2) ? 255 : 0, timestamp,
                 false);
    auto* controller = (*Common::Singleton<Input::GameControllers>::Instance())[0];
    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    controller->SetFrontendState(state);
}

} // namespace

extern "C" RETRO_API void RETRO_CALLCONV retro_unload_game(void);

namespace Libretro {

void SubmitVideoFrame(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                      std::size_t pitch) {
    if (!frontend_active.load(std::memory_order_relaxed) || !rgba || width == 0 || height == 0 ||
        pitch < static_cast<std::size_t>(width) * 4) {
        return;
    }
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<std::size_t>(width) * height);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* row = rgba + static_cast<std::size_t>(y) * pitch;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto* pixel = row + static_cast<std::size_t>(x) * 4;
            frame.pixels[static_cast<std::size_t>(y) * width + x] =
                (static_cast<std::uint32_t>(pixel[0]) << 16) |
                (static_cast<std::uint32_t>(pixel[1]) << 8) | pixel[2];
        }
    }
    std::scoped_lock lock{queue_mutex};
    if (video_queue.size() >= 3) {
        video_queue.pop_front();
    }
    video_queue.emplace_back(std::move(frame));
}

void SubmitAudioSamples(const std::int16_t* samples, std::size_t frames) {
    if (!frontend_active.load(std::memory_order_relaxed) || !samples || frames == 0)
        return;
    std::scoped_lock lock{queue_mutex};
    for (std::size_t i = 0; i < frames * 2; ++i) {
        audio_queue.push_back(samples[i]);
    }
    while (audio_queue.size() > kMaxQueuedAudioFrames * 2) {
        audio_queue.pop_front();
        audio_queue.pop_front();
    }
}

} // namespace Libretro

extern "C" {

RETRO_API unsigned RETRO_CALLCONV retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void RETRO_CALLCONV retro_set_environment(retro_environment_t cb) {
    environment_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_video_refresh(retro_video_refresh_t cb) {
    video_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_audio_sample(retro_audio_sample_t cb) {
    audio_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
    audio_batch_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_input_poll(retro_input_poll_t cb) {
    input_poll_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_input_state(retro_input_state_t cb) {
    input_state_callback = cb;
}
RETRO_API void RETRO_CALLCONV retro_set_controller_port_device(unsigned, unsigned) {}

RETRO_API void RETRO_CALLCONV retro_init(void) {
    KeepCoreLoadedUntilProcessExit();
    frontend_active.store(true, std::memory_order_relaxed);
    if (environment_callback) {
        enum retro_pixel_format format = RETRO_PIXEL_FORMAT_XRGB8888;
        environment_callback(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
        bool support_bitmasks = true;
        environment_callback(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, &support_bitmasks);
        bool no_game = false;
        environment_callback(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);
        static const retro_controller_description controller_types[] = {
            {"DualShock 4", RETRO_DEVICE_JOYPAD},
        };
        static const retro_controller_info controllers[] = {
            {controller_types, 1},
            {nullptr, 0},
        };
        environment_callback(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO,
                             const_cast<retro_controller_info*>(controllers));
        static const retro_input_descriptor inputs[] = {
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "Cross"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "Circle"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Square"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Triangle"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Touchpad"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Options"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L1"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R1"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3"},
            {0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3"},
            {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
             "Left Stick X"},
            {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
             "Left Stick Y"},
            {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
             "Right Stick X"},
            {0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
             "Right Stick Y"},
            {},
        };
        environment_callback(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
                             const_cast<retro_input_descriptor*>(inputs));
    }
}

RETRO_API void RETRO_CALLCONV retro_deinit(void) {
    frontend_active.store(false, std::memory_order_relaxed);
    retro_unload_game();
    environment_callback = nullptr;
    video_callback = nullptr;
    audio_callback = nullptr;
    audio_batch_callback = nullptr;
    input_poll_callback = nullptr;
    input_state_callback = nullptr;
}

RETRO_API void RETRO_CALLCONV retro_get_system_info(struct retro_system_info* info) {
    if (!info)
        return;
    *info = {};
    info->library_name = "shadPS4";
    static const std::string version = std::string{Common::g_version} + "-libretro";
    info->library_version = version.c_str();
    info->valid_extensions = "bin|elf|zar";
    info->need_fullpath = true;
    info->block_extract = true;
}

RETRO_API void RETRO_CALLCONV retro_get_system_av_info(struct retro_system_av_info* info) {
    if (!info)
        return;
    info->geometry = {1920, 1080, 3840, 2160, 16.0f / 9.0f};
    info->timing = {60.0, 48000.0};
}

RETRO_API bool RETRO_CALLCONV retro_load_game(const struct retro_game_info* game) {
    if (game_loaded || session_started || !game || !game->path || !*game->path)
        return false;
    try {
        loaded_content = std::filesystem::u8path(game->path);
        if (std::filesystem::is_directory(loaded_content)) {
            loaded_content /= "eboot.bin";
        }
        if (!std::filesystem::is_regular_file(loaded_content))
            return false;
        const auto save_directory = GetEnvironmentDirectory(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY);
        const auto system_directory =
            GetEnvironmentDirectory(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY);
        if (!save_directory.empty()) {
            SetUserDirectory(save_directory / "shadPS4-libretro");
        }
        if (!system_directory.empty()) {
            const auto sys_modules = system_directory / "shadPS4" / "sys_modules";
            std::filesystem::create_directories(sys_modules);
            Common::FS::SetUserPath(Common::FS::PathType::SysModuleDir, sys_modules);
        }

        IPC::Instance().Init();
        auto state = std::make_shared<EmulatorState>();
        EmulatorState::SetInstance(state);
        Common::Log::Setup("libretro.log");
        KeyManager::GetInstance()->LoadFromFile();
        EmulatorSettings.Load();
        UserSettings.Load();

        auto* emulator = Common::Singleton<Core::Emulator>::Instance();
        emulator->executableName = "shadps4_libretro";
        // Run() starts the guest asynchronously. Mark the process-wide session
        // consumed before entering it, including when game loading fails.
        session_started = true;
        emulator->Run(loaded_content);
        if (!emulator->IsGameStarted()) {
            loaded_content.clear();
            return false;
        }
        game_loaded = true;
        return true;
    } catch (...) {
        loaded_content.clear();
        return false;
    }
}

RETRO_API bool RETRO_CALLCONV retro_load_game_special(unsigned, const struct retro_game_info*,
                                                      size_t) {
    return false;
}

RETRO_API void RETRO_CALLCONV retro_unload_game(void) {
    std::scoped_lock lock{queue_mutex};
    video_queue.clear();
    audio_queue.clear();
    loaded_content.clear();
    game_loaded = false;
}

RETRO_API void RETRO_CALLCONV retro_run(void) {
    PollInput();
    std::deque<VideoFrame> frames;
    std::vector<std::int16_t> samples;
    {
        std::scoped_lock lock{queue_mutex};
        frames.swap(video_queue);
        samples.assign(audio_queue.begin(), audio_queue.end());
        audio_queue.clear();
    }
    if (video_callback) {
        if (frames.empty()) {
            video_callback(nullptr, 0, 0, 0);
        } else {
            const auto& frame = frames.back();
            video_callback(frame.pixels.data(), frame.width, frame.height,
                           static_cast<std::size_t>(frame.width) * sizeof(std::uint32_t));
        }
    }
    if (!samples.empty()) {
        if (audio_batch_callback) {
            const auto frames_sent = audio_batch_callback(samples.data(), samples.size() / 2);
            const auto consumed = std::min(samples.size(), frames_sent * 2);
            if (consumed < samples.size()) {
                std::scoped_lock lock{queue_mutex};
                audio_queue.insert(audio_queue.begin(), samples.begin() + consumed, samples.end());
            }
        } else if (audio_callback) {
            for (std::size_t i = 0; i + 1 < samples.size(); i += 2) {
                audio_callback(samples[i], samples[i + 1]);
            }
        }
    }
}

RETRO_API void RETRO_CALLCONV retro_reset(void) {}
RETRO_API size_t RETRO_CALLCONV retro_serialize_size(void) {
    return 0;
}
RETRO_API bool RETRO_CALLCONV retro_serialize(void*, size_t) {
    return false;
}
RETRO_API bool RETRO_CALLCONV retro_unserialize(const void*, size_t) {
    return false;
}
RETRO_API void RETRO_CALLCONV retro_cheat_reset(void) {}
RETRO_API void RETRO_CALLCONV retro_cheat_set(unsigned, bool, const char*) {}
RETRO_API unsigned RETRO_CALLCONV retro_get_region(void) {
    return RETRO_REGION_NTSC;
}
RETRO_API void* RETRO_CALLCONV retro_get_memory_data(unsigned) {
    return nullptr;
}
RETRO_API size_t RETRO_CALLCONV retro_get_memory_size(unsigned) {
    return 0;
}

} // extern "C"
