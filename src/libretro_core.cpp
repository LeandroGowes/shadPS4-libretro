// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL.h>
#include "libretro.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
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
#include <common/memory_patcher.h>
#include <common/path_util.h>
#include <common/singleton.h>
#include <core/emulator_state.h>
#include <core/ipc/ipc.h>
#include <core/libraries/kernel/time.h>
#include <core/libraries/np/np_trophy.h>
#include <input/controller.h>
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "core/emulator_settings.h"
#include "core/user_settings.h"
#include "emulator.h"
#include "imgui/renderer/imgui_core.h"

namespace {

retro_environment_t environment_callback{};
retro_video_refresh_t video_callback{};
retro_audio_sample_t audio_callback{};
retro_audio_sample_batch_t audio_batch_callback{};
retro_input_poll_t input_poll_callback{};
retro_input_state_t input_state_callback{};
retro_log_printf_t log_callback{};

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
bool motion_mode{};
bool motion_toggle_held{};
bool shake_held{};
u64 shake_until{};
struct MotionQuaternion {
    float w{1.0f};
    float x{};
    float y{};
    float z{};
};
struct MotionSimulation {
    MotionQuaternion orientation{};
    float roll{};
    float yaw{};
    float pitch{};
    u64 last_update{};
};
MotionSimulation motion_simulation{};
constexpr unsigned kSecondTouchButton = 16;
constexpr unsigned kFirstTouchButton = 17;
constexpr unsigned kPhysicalMotionButton = 18;
constexpr unsigned kSimulatedMotionButton = 19;
constexpr unsigned kMouseAimButton = 20;
constexpr unsigned kRecenterMotionButton = 21;
SDL_Gamepad* sensor_gamepad{};
bool sensor_subsystem{};
u64 next_sensor_scan{};
MotionQuaternion physical_orientation{};
bool physical_orientation_initialized{};
bool recenter_held{};
std::atomic_bool physical_sensor_available{};
std::atomic<unsigned> active_motion_source{};
constexpr std::size_t kMaxQueuedAudioFrames = 48000 * 2;

MotionQuaternion Multiply(const MotionQuaternion& a, const MotionQuaternion& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}

MotionQuaternion Normalize(MotionQuaternion q) {
    const float norm = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (norm <= 0.0f)
        return {};
    q.w /= norm;
    q.x /= norm;
    q.y /= norm;
    q.z /= norm;
    return q;
}

float AimCurve(float value) {
    constexpr float dead_zone = 0.15f;
    const float magnitude = std::abs(value);
    if (magnitude <= dead_zone)
        return 0.0f;
    const float scaled = std::clamp((magnitude - dead_zone) / (1.0f - dead_zone), 0.0f, 1.0f);
    return value < 0.0f ? -scaled * scaled : scaled * scaled;
}

std::array<float, 3> GravityFromOrientation(const MotionQuaternion& q) {
    const MotionQuaternion conjugate{q.w, -q.x, -q.y, -q.z};
    const MotionQuaternion world_up{0.0f, 0.0f, 1.0f, 0.0f};
    const auto result = Multiply(Multiply(conjugate, world_up), q);
    return {result.x, result.y, result.z};
}

bool ReadPhysicalSensors(float* gyro, float* acceleration) {
    if (!sensor_subsystem)
        return false;
    SDL_UpdateGamepads();
    if (sensor_gamepad && !SDL_GamepadConnected(sensor_gamepad)) {
        SDL_CloseGamepad(sensor_gamepad);
        sensor_gamepad = nullptr;
        physical_orientation_initialized = false;
    }
    const auto now = SDL_GetTicks();
    if (!sensor_gamepad && now >= next_sensor_scan) {
        next_sensor_scan = now + 1000;
        int count{};
        auto* ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i) {
            auto* candidate = SDL_OpenGamepad(ids[i]);
            if (!candidate)
                continue;
            if (SDL_GamepadHasSensor(candidate, SDL_SENSOR_GYRO) &&
                SDL_GamepadHasSensor(candidate, SDL_SENSOR_ACCEL) &&
                SDL_SetGamepadSensorEnabled(candidate, SDL_SENSOR_GYRO, true) &&
                SDL_SetGamepadSensorEnabled(candidate, SDL_SENSOR_ACCEL, true)) {
                sensor_gamepad = candidate;
                break;
            }
            SDL_CloseGamepad(candidate);
        }
        SDL_free(ids);
    }
    const bool available =
        sensor_gamepad && SDL_GetGamepadSensorData(sensor_gamepad, SDL_SENSOR_GYRO, gyro, 3) &&
        SDL_GetGamepadSensorData(sensor_gamepad, SDL_SENSOR_ACCEL, acceleration, 3);
    const float magnitude = acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1] +
                            acceleration[2] * acceleration[2];
    const bool valid = available && std::isfinite(magnitude) && magnitude > 0.01f &&
                       std::isfinite(gyro[0]) && std::isfinite(gyro[1]) && std::isfinite(gyro[2]);
    physical_sensor_available.store(valid);
    return valid;
}

MotionQuaternion PhysicalOrientation(const float* gyro, const float* acceleration, float dt) {
    const float length =
        std::sqrt(acceleration[0] * acceleration[0] + acceleration[1] * acceleration[1] +
                  acceleration[2] * acceleration[2]);
    const float ax = acceleration[0] / length;
    const float ay = acceleration[1] / length;
    const float az = acceleration[2] / length;
    if (!physical_orientation_initialized) {
        physical_orientation =
            ay < -0.999f ? MotionQuaternion{0, 1, 0, 0} : Normalize({1.0f + ay, -az, 0, ax});
        physical_orientation_initialized = true;
    }
    auto adjusted = std::array<float, 3>{gyro[0], gyro[1], gyro[2]};
    if (length > 7.8f && length < 11.8f) {
        const auto gravity = GravityFromOrientation(physical_orientation);
        adjusted[0] += 1.5f * (ay * gravity[2] - az * gravity[1]);
        adjusted[1] += 1.5f * (az * gravity[0] - ax * gravity[2]);
        adjusted[2] += 1.5f * (ax * gravity[1] - ay * gravity[0]);
    }
    const auto delta = Multiply(physical_orientation, {0, adjusted[0], adjusted[1], adjusted[2]});
    physical_orientation = Normalize({physical_orientation.w + delta.w * dt * 0.5f,
                                      physical_orientation.x + delta.x * dt * 0.5f,
                                      physical_orientation.y + delta.y * dt * 0.5f,
                                      physical_orientation.z + delta.z * dt * 0.5f});
    return physical_orientation;
}

static const retro_variable core_options[] = {
    {"shadps4_neo_mode", "Console: PS4 Pro mode (restart game); native|disabled|enabled"},
    {"shadps4_devkit_mode", "Console: developer mode (restart game); native|disabled|enabled"},
    {"shadps4_console_language",
     "Console: language (PS4 ID) (restart game); "
     "native|1|17|0|2|3|4|5|6|7|8|9|10|11|12|13|14|15|16|18|19|20|21|22|23|24|25|26|27|28|29"},
    {"shadps4_circle_enter",
     "Controls: confirm with Circle (restart game); native|disabled|enabled"},
    {"shadps4_readbacks", "GPU: memory readbacks (restart game); native|disabled|relaxed|precise"},
    {"shadps4_readback_linear",
     "GPU: linear image readbacks (restart game); native|disabled|enabled"},
    {"shadps4_direct_memory", "GPU: direct memory access (restart game); native|disabled|enabled"},
    {"shadps4_copy_gpu_buffers", "GPU: copy buffers (restart game); native|disabled|enabled"},
    {"shadps4_gpu", "Vulkan: device (index) (restart game); native|auto|0|1|2|3"},
    {"shadps4_pipeline_cache", "Vulkan: pipeline cache (restart game); native|disabled|enabled"},
    {"shadps4_validation",
     "Diagnostics: Vulkan validation (restart game); native|disabled|enabled"},
    {"shadps4_validation_sync",
     "Diagnostics: Vulkan synchronization validation (restart game); native|disabled|enabled"},
    {"shadps4_dump_shaders",
     "Diagnostics: dump shaders to disk (restart game); native|disabled|enabled"},
    {"shadps4_shader_collect",
     "Diagnostics: collect shaders (restart game); native|disabled|enabled"},
    {"shadps4_debug_dump",
     "Diagnostics: dump executables to disk (restart game); native|disabled|enabled"},
    {"shadps4_auto_patches",
     "Patches: automatically load installed game patches (restart game); native|enabled|disabled"},
#ifdef _WIN32
    {"shadps4_redzone_patching",
     "CPU: Windows guest red-zone patching (restart game); native|disabled|enabled"},
#endif
#ifdef __linux__
    {"shadps4_userfaultfd", "Memory: userfaultfd tracking (restart game); native|disabled|enabled"},
#endif
    {nullptr, nullptr},
};

std::string_view GetCoreOption(const char* key) {
    retro_variable variable{key, nullptr};
    if (!environment_callback || !environment_callback(RETRO_ENVIRONMENT_GET_VARIABLE, &variable) ||
        !variable.value) {
        return "native";
    }
    return variable.value;
}

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

    u32 ui_buttons = 0;
    for (u32 bit = 0; bit < 16; ++bit) {
        if (input_state_callback(0, RETRO_DEVICE_JOYPAD, 0, bit)) {
            ui_buttons |= u32{1} << bit;
        }
    }
    ImGui::Core::SubmitFrontendInput(
        ui_buttons,
        input_state_callback(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                             RETRO_DEVICE_ID_ANALOG_X),
        input_state_callback(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                             RETRO_DEVICE_ID_ANALOG_Y));

    using Button = Libraries::Pad::OrbisPadButtonDataOffset;
    Input::State state{};
    const u64 timestamp = Libraries::Kernel::sceKernelGetProcessTime();
    const auto pressed = [](unsigned id) {
        return input_state_callback(0, RETRO_DEVICE_JOYPAD, 0, id) != 0;
    };
    float physical_gyro[3]{};
    float physical_acceleration[3]{};
    const bool physical_available = ReadPhysicalSensors(physical_gyro, physical_acceleration);
    const bool physical_only = pressed(kPhysicalMotionButton);
    const bool simulation_only = pressed(kSimulatedMotionButton);
    const bool mouse_aim = pressed(kMouseAimButton);
    const bool cross = pressed(RETRO_DEVICE_ID_JOYPAD_B);
    const bool touch = pressed(kFirstTouchButton);
    const bool second_touch = touch && pressed(kSecondTouchButton);
    const bool motion_toggle =
        pressed(RETRO_DEVICE_ID_JOYPAD_L3) && pressed(RETRO_DEVICE_ID_JOYPAD_R3);
    if (motion_toggle && !motion_toggle_held && !physical_only) {
        motion_mode = !motion_mode;
    }
    motion_toggle_held = motion_toggle;
    const bool simulate_motion = mouse_aim || (!physical_only && motion_mode);
    const bool use_physical = physical_available && !simulation_only && !simulate_motion;
    const bool recenter = pressed(kRecenterMotionButton);
    if (recenter && !recenter_held) {
        physical_orientation_initialized = false;
        motion_simulation.yaw = 0;
        motion_simulation.pitch = 0;
    }
    recenter_held = recenter;
    active_motion_source.store(use_physical ? 1 : simulate_motion ? 2 : 0);
    state.OnButton(Button::Cross, cross && !simulate_motion);
    state.OnButton(Button::Circle, pressed(RETRO_DEVICE_ID_JOYPAD_A));
    state.OnButton(Button::Square, pressed(RETRO_DEVICE_ID_JOYPAD_Y));
    state.OnButton(Button::Triangle, pressed(RETRO_DEVICE_ID_JOYPAD_X));
    state.OnButton(
        Button::TouchPad,
        pressed(RETRO_DEVICE_ID_JOYPAD_SELECT) ||
            (sensor_gamepad && SDL_GetGamepadButton(sensor_gamepad, SDL_GAMEPAD_BUTTON_TOUCHPAD)));
    state.OnButton(Button::Options, pressed(RETRO_DEVICE_ID_JOYPAD_START));
    state.OnButton(Button::Up, pressed(RETRO_DEVICE_ID_JOYPAD_UP));
    state.OnButton(Button::Down, pressed(RETRO_DEVICE_ID_JOYPAD_DOWN));
    state.OnButton(Button::Left, pressed(RETRO_DEVICE_ID_JOYPAD_LEFT));
    state.OnButton(Button::Right, pressed(RETRO_DEVICE_ID_JOYPAD_RIGHT));
    state.OnButton(Button::L1, pressed(RETRO_DEVICE_ID_JOYPAD_L));
    state.OnButton(Button::R1, pressed(RETRO_DEVICE_ID_JOYPAD_R));
    state.OnButton(Button::L2, pressed(RETRO_DEVICE_ID_JOYPAD_L2));
    state.OnButton(Button::R2, pressed(RETRO_DEVICE_ID_JOYPAD_R2));
    // The L3+R3 chord belongs to the frontend motion-mode toggle. Keep that
    // chord from triggering an unrelated in-game action while toggling.
    state.OnButton(Button::L3, pressed(RETRO_DEVICE_ID_JOYPAD_L3) && !motion_toggle);
    state.OnButton(Button::R3, pressed(RETRO_DEVICE_ID_JOYPAD_R3) && !motion_toggle);

    const auto axis = [](unsigned index, unsigned axis_id) {
        if (!input_state_callback)
            return 128;
        const int raw = input_state_callback(0, RETRO_DEVICE_ANALOG, index, axis_id);
        return (raw + 32768) / 256;
    };
    state.OnAxis(Input::Axis::LeftX,
                 simulate_motion || second_touch
                     ? 128
                     : axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X),
                 timestamp, false);
    state.OnAxis(Input::Axis::LeftY,
                 simulate_motion || second_touch
                     ? 128
                     : axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y),
                 timestamp, false);
    state.OnAxis(Input::Axis::RightX,
                 (touch || mouse_aim)
                     ? 128
                     : axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X),
                 timestamp, false);
    state.OnAxis(Input::Axis::RightY,
                 (touch || mouse_aim)
                     ? 128
                     : axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y),
                 timestamp, false);
    state.OnAxis(Input::Axis::TriggerLeft, pressed(RETRO_DEVICE_ID_JOYPAD_L2) ? 255 : 0, timestamp,
                 false);
    state.OnAxis(Input::Axis::TriggerRight, pressed(RETRO_DEVICE_ID_JOYPAD_R2) ? 255 : 0, timestamp,
                 false);
    auto* controller = (*Common::Singleton<Input::GameControllers>::Instance())[0];
    const auto raw_axis = [](unsigned index, unsigned axis_id) {
        return input_state_callback(0, RETRO_DEVICE_ANALOG, index, axis_id);
    };
    const float lx =
        static_cast<float>(raw_axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X)) /
        32768.0f;
    const float ly =
        static_cast<float>(raw_axis(RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y)) /
        32768.0f;
    const float rx =
        static_cast<float>(raw_axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X)) /
        32768.0f;
    const float ry =
        static_cast<float>(raw_axis(RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y)) /
        32768.0f;
    const float touch_x = std::clamp(0.5f + rx * 0.5f, 0.0f, 1.0f);
    const float touch_y = std::clamp(0.5f + ry * 0.5f, 0.0f, 1.0f);
    const float second_touch_x = std::clamp(0.5f + lx * 0.5f, 0.0f, 1.0f);
    const float second_touch_y = std::clamp(0.5f + ly * 0.5f, 0.0f, 1.0f);

    const u64 motion_elapsed =
        motion_simulation.last_update == 0 || timestamp <= motion_simulation.last_update
            ? 0
            : timestamp - motion_simulation.last_update;
    const float dt = std::min(static_cast<float>(motion_elapsed) / 1'000'000.0f, 0.1f);
    motion_simulation.last_update = timestamp;
    const float ease = dt > 0.0f ? 1.0f - std::exp(-dt * 12.0f) : 1.0f;
    const float target_roll = simulate_motion ? 1.57079632679f : 0.0f;
    motion_simulation.roll += (target_roll - motion_simulation.roll) * ease;
    if (simulate_motion) {
        if (mouse_aim) {
            motion_simulation.yaw = -rx * 1.4f;
            motion_simulation.pitch = -ry * 1.2f;
        } else if (!second_touch) {
            motion_simulation.yaw -= AimCurve(lx) * 1.2f * dt;
            motion_simulation.pitch -= AimCurve(ly) * 1.2f * dt;
        }
        motion_simulation.yaw = std::clamp(motion_simulation.yaw, -1.4f, 1.4f);
        motion_simulation.pitch = std::clamp(motion_simulation.pitch, -1.2f, 1.2f);
        if (cross && !shake_held) {
            shake_until = timestamp + 180'000;
        }
    } else {
        motion_simulation.yaw -= motion_simulation.yaw * ease;
        motion_simulation.pitch -= motion_simulation.pitch * ease;
    }
    shake_held = cross;

    const float half_roll = motion_simulation.roll * 0.5f;
    const float half_pitch = motion_simulation.pitch * 0.5f;
    const float half_yaw = motion_simulation.yaw * 0.5f;
    const MotionQuaternion roll_q{std::cos(half_roll), 0.0f, 0.0f, std::sin(-half_roll)};
    const MotionQuaternion pitch_q{std::cos(half_pitch), std::sin(half_pitch), 0.0f, 0.0f};
    const MotionQuaternion yaw_q{std::cos(half_yaw), 0.0f, std::sin(half_yaw), 0.0f};
    MotionQuaternion orientation = Normalize(Multiply(yaw_q, Multiply(pitch_q, roll_q)));
    const auto old_inverse =
        MotionQuaternion{motion_simulation.orientation.w, -motion_simulation.orientation.x,
                         -motion_simulation.orientation.y, -motion_simulation.orientation.z};
    const auto delta_orientation = Multiply(old_inverse, orientation);
    float gyro[3]{};
    if (dt > 0.0f) {
        const float sign = delta_orientation.w < 0.0f ? -2.0f : 2.0f;
        gyro[0] = sign * delta_orientation.x / dt;
        gyro[1] = sign * delta_orientation.y / dt;
        gyro[2] = sign * delta_orientation.z / dt;
    }
    motion_simulation.orientation = orientation;

    float acceleration[3]{};
    const auto gravity = GravityFromOrientation(orientation);
    for (int i = 0; i < 3; ++i) {
        acceleration[i] = gravity[i] * 9.81f;
    }
    if (timestamp < shake_until) {
        constexpr float two_pi = 6.28318530718f;
        const float phase = static_cast<float>(timestamp % 1'000'000) * 1.0e-6f * two_pi * 7.0f;
        const float shake = std::sin(phase);
        const float wobble = 6.0f * std::cos(phase);
        for (int i = 0; i < 3; ++i) {
            acceleration[i] += 2.5f * shake * gravity[i] * 9.81f;
        }
        gyro[0] += wobble * gravity[1];
        gyro[1] -= wobble * gravity[0];
    }
    if (physical_available) {
        const auto physical = PhysicalOrientation(physical_gyro, physical_acceleration, dt);
        if (use_physical) {
            orientation = physical;
            std::copy_n(physical_gyro, 3, gyro);
            std::copy_n(physical_acceleration, 3, acceleration);
        }
    }
    state.orientation = {orientation.x, orientation.y, orientation.z, orientation.w};
    controller->UpdateGyro(gyro);
    controller->UpdateAcceleration(acceleration);
    state.time = Libraries::Kernel::sceKernelGetProcessTime();
    state.OnTouchpad(0, touch, touch_x, touch_y);
    state.OnTouchpad(1, second_touch, second_touch_x, second_touch_y);
    if (!touch && sensor_gamepad) {
        for (int finger = 0; finger < 2; ++finger) {
            bool down{};
            float x{}, y{}, pressure{};
            if (SDL_GetGamepadTouchpadFinger(sensor_gamepad, 0, finger, &down, &x, &y, &pressure)) {
                state.OnTouchpad(finger, down, x, y);
            }
        }
    }
    controller->SetFrontendState(state);
}

} // namespace

extern "C" RETRO_API void RETRO_CALLCONV retro_unload_game(void);

namespace Libretro {

void ApplyCoreOptions() {
    if (const auto value = GetCoreOption("shadps4_neo_mode"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetNeo(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_neo_mode: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_devkit_mode"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetDevKit(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_devkit_mode: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_console_language"); value != "native") {
        if (value == "1" || value == "17" || value == "0" || value == "2" || value == "3" ||
            value == "4" || value == "5" || value == "6" || value == "7" || value == "8" ||
            value == "9" || value == "10" || value == "11" || value == "12" || value == "13" ||
            value == "14" || value == "15" || value == "16" || value == "18" || value == "19" ||
            value == "20" || value == "21" || value == "22" || value == "23" || value == "24" ||
            value == "25" || value == "26" || value == "27" || value == "28" || value == "29") {
            EmulatorSettings.SetConsoleLanguage(std::stoi(std::string{value}), true);
            LOG_INFO(Config, "Libretro option shadps4_console_language: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_circle_enter"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetCircleEnter(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_circle_enter: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_readbacks"); value != "native") {
        if (value == "disabled" || value == "relaxed" || value == "precise") {
            EmulatorSettings.SetReadbacksMode(value == "precise"   ? GpuReadbacksMode::Precise
                                              : value == "relaxed" ? GpuReadbacksMode::Relaxed
                                                                   : GpuReadbacksMode::Disabled,
                                              true);
            LOG_INFO(Config, "Libretro option shadps4_readbacks: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_readback_linear"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetReadbackLinearImagesEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_readback_linear: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_direct_memory"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetDirectMemoryAccessEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_direct_memory: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_copy_gpu_buffers"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetCopyGpuBuffers(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_copy_gpu_buffers: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_gpu"); value != "native") {
        if (value == "auto" || value == "0" || value == "1" || value == "2" || value == "3") {
            EmulatorSettings.SetGpuId(value == "auto" ? -1 : std::stoi(std::string{value}), true);
            LOG_INFO(Config, "Libretro option shadps4_gpu: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_pipeline_cache"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetPipelineCacheEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_pipeline_cache: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_validation"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetVkValidationEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_validation: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_validation_sync"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetVkValidationSyncEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_validation_sync: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_dump_shaders"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetDumpShaders(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_dump_shaders: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_shader_collect"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetShaderCollect(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_shader_collect: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_debug_dump"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetDebugDump(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_debug_dump: {}", value);
        }
    }
    if (const auto value = GetCoreOption("shadps4_auto_patches"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorState::GetInstance()->SetAutoPatchesLoadEnabled(value == "enabled");
            LOG_INFO(Config, "Libretro option shadps4_auto_patches: {}", value);
        }
    }
#ifdef _WIN32
    if (const auto value = GetCoreOption("shadps4_redzone_patching"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetRedZonePatchingEnabled(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_redzone_patching: {}", value);
        }
    }
#endif
#ifdef __linux__
    if (const auto value = GetCoreOption("shadps4_userfaultfd"); value != "native") {
        if (value == "enabled" || value == "disabled") {
            EmulatorSettings.SetUserfaultfdTracking(value == "enabled", true);
            LOG_INFO(Config, "Libretro option shadps4_userfaultfd: {}", value);
        }
    }
#endif
}

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
    if (cb) {
        cb(RETRO_ENVIRONMENT_SET_VARIABLES, const_cast<retro_variable*>(core_options));
    }
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
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    sensor_subsystem = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    if (sensor_subsystem) {
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
    }
    frontend_active.store(true, std::memory_order_relaxed);
    if (environment_callback) {
        retro_log_callback logging{};
        if (environment_callback(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging)) {
            log_callback = logging.log;
        }
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

extern "C" RETRO_API const char* promus_libretro_motion_status() {
    if (!game_loaded && sensor_subsystem) {
        float gyro[3]{}, acceleration[3]{};
        ReadPhysicalSensors(gyro, acceleration);
    }
    thread_local std::string json;
    const auto source = active_motion_source.load();
    json = std::string{"{\"available\":"} + (physical_sensor_available.load() ? "true" : "false") +
           ",\"backendReady\":" + (sensor_subsystem ? "true" : "false") + ",\"source\":\"" +
           (source == 1   ? "physical"
            : source == 2 ? "simulated"
                          : "idle") +
           "\"}";
    return json.c_str();
}

RETRO_API void RETRO_CALLCONV retro_deinit(void) {
    if (sensor_gamepad) {
        SDL_CloseGamepad(sensor_gamepad);
        sensor_gamepad = nullptr;
    }
    if (sensor_subsystem) {
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        sensor_subsystem = false;
    }
    physical_sensor_available.store(false);
    frontend_active.store(false, std::memory_order_relaxed);
    retro_unload_game();
    environment_callback = nullptr;
    video_callback = nullptr;
    audio_callback = nullptr;
    audio_batch_callback = nullptr;
    input_poll_callback = nullptr;
    input_state_callback = nullptr;
    log_callback = nullptr;
}

RETRO_API void RETRO_CALLCONV retro_get_system_info(struct retro_system_info* info) {
    if (!info)
        return;
    *info = {};
    info->library_name = "shadPS4 (Libretro - Promus)";
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
    motion_mode = false;
    motion_toggle_held = false;
    shake_held = false;
    shake_until = 0;
    motion_simulation = {};
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
    } catch (const std::exception& error) {
        if (log_callback) {
            log_callback(RETRO_LOG_ERROR, "shadPS4: %s", error.what());
        }
        loaded_content.clear();
        return false;
    } catch (...) {
        if (log_callback) {
            log_callback(RETRO_LOG_ERROR, "shadPS4: unknown failure while loading content");
        }
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
RETRO_API void RETRO_CALLCONV retro_cheat_set(unsigned, bool enabled, const char* code) {
    if (!code || !*code) {
        return;
    }
    const std::string_view cheat_code{code};
    const auto separator = cheat_code.rfind('|');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= cheat_code.size()) {
        return;
    }
    try {
        const auto index = std::stoull(std::string{cheat_code.substr(separator + 1)});
        MemoryPatcher::ApplyCheatMod(std::string{cheat_code.substr(0, separator)}, index, enabled);
    } catch (const std::exception&) {
        if (log_callback) {
            log_callback(RETRO_LOG_WARN, "shadPS4: ignored malformed cheat command");
        }
    }
}
extern "C" RETRO_API const char* RETRO_CALLCONV promus_libretro_get_trophies_json(void) {
    static std::string trophies_json;
    trophies_json = Libraries::Np::NpTrophy::GetTrophiesJson();
    return trophies_json.c_str();
}
extern "C" RETRO_API bool RETRO_CALLCONV promus_libretro_inspect_trophies(
    const char* game_path, const char* user_directory, const char* title_id,
    const char* output_path, char* error, std::size_t error_capacity) {
    const auto set_error = [error, error_capacity](std::string_view message) {
        if (!error || error_capacity == 0)
            return;
        const auto size = std::min(error_capacity - 1, message.size());
        std::memcpy(error, message.data(), size);
        error[size] = '\0';
    };
    try {
        if (!game_path || !*game_path || !user_directory || !*user_directory || !title_id ||
            !*title_id || !output_path || !*output_path) {
            set_error("Parâmetros ausentes para consultar troféus.");
            return false;
        }
        SetUserDirectory(std::filesystem::u8path(user_directory));
        KeyManager::GetInstance()->LoadFromFile();
        if (KeyManager::GetInstance()->GetAllKeys().TrophyKeySet.ReleaseTrophyKey.size() != 16) {
            set_error("Configure a chave de troféus do shadPS4 para ler os pacotes deste jogo.");
            return false;
        }
        EmulatorSettings.Load(title_id);
        UserSettings.Load();
        const auto json = Core::InspectTrophies(std::filesystem::u8path(game_path), title_id);
        const auto destination = std::filesystem::u8path(output_path);
        std::filesystem::create_directories(destination.parent_path());
        auto temporary = destination;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output || !output.write(json.data(), static_cast<std::streamsize>(json.size()))) {
                set_error("Não foi possível gravar o cache de troféus.");
                return false;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temporary, destination, ec);
        if (ec) {
            std::filesystem::remove(destination, ec);
            ec.clear();
            std::filesystem::rename(temporary, destination, ec);
        }
        if (ec) {
            set_error("Não foi possível finalizar o cache de troféus.");
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        set_error(exception.what());
        return false;
    } catch (...) {
        set_error("Falha desconhecida ao consultar troféus.");
        return false;
    }
}
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
