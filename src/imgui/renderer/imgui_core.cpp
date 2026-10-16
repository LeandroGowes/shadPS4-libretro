// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <SDL3/SDL_events.h>
#include <imgui.h>

#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "font_data.h"
#include "font_stack.h"
#include "imgui/imgui_layer.h"
#include "imgui_core.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_internal.h"
#include "sdl_window.h"
#include "texture_manager.h"
#include "video_core/renderer_vulkan/vk_presenter.h"

static void CheckVkResult(const vk::Result err) {
    LOG_ERROR(ImGui, "Vulkan error {}", vk::to_string(err));
}

static std::vector<ImGui::Layer*> layers;

// Update layers before rendering to allow layer changes to be applied during rendering.
// Using deque to keep the order of changes in case a Layer is removed then added again between
// frames.
static std::deque<std::pair<bool, ImGui::Layer*>> change_layers{};
static std::mutex change_layers_mutex{};

static ImGuiID dock_id;
static std::atomic<std::uint32_t> force_gamepad_input_capture_count{0};
#if defined(LIBRETRO_CORE)
static std::atomic<u64> frontend_input{};
static std::atomic_bool frontend_input_captured{};
static auto frontend_frame_time = std::chrono::steady_clock::now();
#endif

namespace ImGui {

namespace Core {

void AcquireGamepadInputCapture() {
    force_gamepad_input_capture_count.fetch_add(1, std::memory_order_relaxed);
}

void ReleaseGamepadInputCapture() {
    std::uint32_t expected = force_gamepad_input_capture_count.load(std::memory_order_relaxed);
    while (expected != 0) {
        if (force_gamepad_input_capture_count.compare_exchange_weak(
                expected, expected - 1, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
    LOG_WARNING(ImGui, "ReleaseGamepadInputCapture called with no active capture");
}

bool IsGamepadInputCaptured() {
#if defined(LIBRETRO_CORE)
    if (frontend_input_captured.load(std::memory_order_relaxed)) {
        return true;
    }
#endif
    return force_gamepad_input_capture_count.load(std::memory_order_relaxed) > 0;
}

#if defined(LIBRETRO_CORE)
void SubmitFrontendInput(u32 buttons, s16 left_x, s16 left_y) {
    frontend_input.store(static_cast<u64>(buttons) |
                             (static_cast<u64>(static_cast<u16>(left_x)) << 32) |
                             (static_cast<u64>(static_cast<u16>(left_y)) << 48),
                         std::memory_order_relaxed);
}
#endif

void Initialize(const ::Vulkan::Instance& instance, const Frontend::WindowSDL& window,
                const u32 image_count, vk::Format surface_format,
                const vk::AllocationCallbacks* allocator) {

    const auto config_path = GetUserPath(Common::FS::PathType::UserDir) / "imgui.ini";
    const auto log_path = GetUserPath(Common::FS::PathType::LogDir) / "imgui_log.txt";

    CreateContext();
    ImGuiIO& io = GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2((float)window.GetWidth(), (float)window.GetHeight());
    PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f); // Makes the window edges rounded

    auto path = config_path.u8string();
    char* config_file_buf = new char[path.size() + 1]();
    std::memcpy(config_file_buf, path.c_str(), path.size());
    io.IniFilename = config_file_buf;

    path = log_path.u8string();
    char* log_file_buf = new char[path.size() + 1]();
    std::memcpy(log_file_buf, path.c_str(), path.size());
    io.LogFilename = log_file_buf;

    ImFontConfig font_cfg{};
    font_cfg.OversampleH = 2;
    font_cfg.OversampleV = 1;
    io.Fonts->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    const int console_language = EmulatorSettings.GetConsoleLanguage();
    io.FontDefault = FontStack::AddPrimaryUiFont(io.Fonts, 32.0f, console_language, font_cfg, true);

    io.Fonts->AddFontFromMemoryCompressedTTF(imgui_font_proggyvector_regular_compressed_data,
                                             imgui_font_proggyvector_regular_compressed_size,
                                             32.0f);

    // Avoid exploding atlas size on Metal/MoltenVK when CJK fallback is enabled.
    FontStack::AddPrimaryUiFont(io.Fonts, 128.0f, console_language, font_cfg, false);

    // Big Picture
    FontStack::AddPrimaryUiFont(ImGui::GetIO().Fonts, 64.0f, EmulatorSettings.GetConsoleLanguage(),
                                font_cfg, true);

    // Let the atlas size grow to the largest image the device can create,
    // floored to the power of two the packer requires.
    const u32 max_dim = instance.GetPhysicalDevice().getProperties().limits.maxImageDimension2D;
    const int atlas_max = static_cast<int>(std::bit_floor(std::max<u32>(max_dim, 512u)));
    io.Fonts->TexMaxWidth = atlas_max;
    io.Fonts->TexMaxHeight = atlas_max;

    io.FontGlobalScale = 0.5f;

    StyleColorsDark();

    ::Core::Devtools::Layer::SetupSettings();
    if (window.GetSDLWindow()) {
        Sdl::Init(window.GetSDLWindow());
    }

    const Vulkan::InitInfo vk_info{
        .instance = instance.GetInstance(),
        .physical_device = instance.GetPhysicalDevice(),
        .device = instance.GetDevice(),
        .queue_family = instance.GetPresentQueueFamilyIndex(),
        .queue = instance.GetPresentQueue(),
        .image_count = image_count,
        .min_allocation_size = 1024 * 1024,
        .pipeline_rendering_create_info{
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &surface_format,
        },
        .allocator = allocator,
        .check_vk_result_fn = &CheckVkResult,
    };
    Vulkan::Init(vk_info);

    TextureManager::StartWorker();

    char label[32];
    ImFormatString(label, IM_ARRAYSIZE(label), "WindowOverViewport_%08X", GetMainViewport()->ID);
    dock_id = ImHashStr(label);

    if (window.GetSDLWindow()) {
        if (const auto dpi = SDL_GetWindowDisplayScale(window.GetSDLWindow()); dpi > 0.0f) {
            GetIO().FontGlobalScale *= dpi;
        }
    }

    std::at_quick_exit([] { SaveIniSettingsToDisk(GetIO().IniFilename); });
}

void OnResize() {
    Sdl::OnResize();
}

void OnSurfaceFormatChange(vk::Format surface_format) {
    Vulkan::OnSurfaceFormatChange(surface_format);
}

void Shutdown(const vk::Device& device) {
    auto result = device.waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on shutdown: {}",
                    vk::to_string(result));
    }

    TextureManager::StopWorker();

    const ImGuiIO& io = GetIO();
    const auto ini_filename = (void*)io.IniFilename;
    const auto log_filename = (void*)io.LogFilename;

    Vulkan::Shutdown();
#if !defined(LIBRETRO_CORE)
    Sdl::Shutdown();
#else
    frontend_input_captured.store(false, std::memory_order_relaxed);
#endif
    DestroyContext();

    delete[] (char*)ini_filename;
    delete[] (char*)log_filename;
}

bool ProcessEvent(SDL_Event* event) {
    Sdl::ProcessEvent(event);
    switch (event->type) {
    // Don't block release/up events
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const auto& io = GetIO();
        return io.WantCaptureMouse && io.Ctx->NavWindow != nullptr &&
               (io.Ctx->NavWindow->Flags & ImGuiWindowFlags_NoNav) == 0;
    }
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN: {
        if (IsGamepadInputCaptured()) {
            // Keep keyboard events flowing through the regular input-binding path while IME/OSK
            // captures gamepad input, so keyboard equivalents of pad buttons still update the
            // virtual controller state.
            return false;
        }
        const auto& io = GetIO();
        return io.WantCaptureKeyboard && io.Ctx->NavWindow != nullptr &&
               io.Ctx->NavWindow->ID != dock_id;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE: {
        if (IsGamepadInputCaptured()) {
            // Let controller events continue to input bindings so OSK shortcuts and virtual
            // controller state keep updating; game-side pad reads are blocked in libScePad.
            return false;
        }
        const auto& io = GetIO();
        return io.NavActive && io.Ctx->NavWindow != nullptr && io.Ctx->NavWindow->ID != dock_id;
    }
    default:
        return false;
    }
}

ImGuiID NewFrame(bool is_reusing_frame) {
    {
        std::scoped_lock lock{change_layers_mutex};
        while (!change_layers.empty()) {
            const auto [to_be_added, layer] = change_layers.front();
            if (to_be_added) {
                layers.push_back(layer);
            } else {
                const auto [begin, end] = std::ranges::remove(layers, layer);
                layers.erase(begin, end);
            }
            change_layers.pop_front();
        }
    }

#if defined(LIBRETRO_CORE)
    auto& io = GetIO();
    const auto now = std::chrono::steady_clock::now();
    io.DeltaTime =
        std::clamp(std::chrono::duration<float>(now - frontend_frame_time).count(), 0.001f, 0.1f);
    frontend_frame_time = now;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    const u64 input = frontend_input.load(std::memory_order_relaxed);
    const std::pair<ImGuiKey, u32> buttons[] = {
        {ImGuiKey_GamepadFaceDown, 0},  {ImGuiKey_GamepadFaceLeft, 1},
        {ImGuiKey_GamepadBack, 2},      {ImGuiKey_GamepadStart, 3},
        {ImGuiKey_GamepadDpadUp, 4},    {ImGuiKey_GamepadDpadDown, 5},
        {ImGuiKey_GamepadDpadLeft, 6},  {ImGuiKey_GamepadDpadRight, 7},
        {ImGuiKey_GamepadFaceRight, 8}, {ImGuiKey_GamepadFaceUp, 9},
        {ImGuiKey_GamepadL1, 10},       {ImGuiKey_GamepadR1, 11},
        {ImGuiKey_GamepadL2, 12},       {ImGuiKey_GamepadR2, 13},
        {ImGuiKey_GamepadL3, 14},       {ImGuiKey_GamepadR3, 15},
    };
    for (const auto& [key, bit] : buttons) {
        io.AddKeyEvent(key, (input & (u64{1} << bit)) != 0);
    }
    const float x = static_cast<s16>(input >> 32) / 32768.0f;
    const float y = static_cast<s16>(input >> 48) / 32768.0f;
    const auto axis = [&io](ImGuiKey key, float value) {
        const float amount = std::clamp((value - 0.24f) / 0.76f, 0.0f, 1.0f);
        io.AddKeyAnalogEvent(key, amount > 0, amount);
    };
    axis(ImGuiKey_GamepadLStickLeft, -x);
    axis(ImGuiKey_GamepadLStickRight, x);
    axis(ImGuiKey_GamepadLStickUp, -y);
    axis(ImGuiKey_GamepadLStickDown, y);
#else
    Sdl::NewFrame(is_reusing_frame);
#endif
    ImGui::NewFrame();
    SetKeyOwner(ImGuiKey_GamepadFaceUp, ImHashStr("shadps4/pad"));

    ImGuiWindowFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode | ImGuiDockNodeFlags_AutoHideTabBar;
    if (!DebugState.IsShowingDebugMenuBar()) {
        flags |= ImGuiDockNodeFlags_NoTabBar;
    }
    ImGuiID dockId = DockSpaceOverViewport(0, GetMainViewport(), flags);

    for (auto* layer : layers) {
        layer->Draw();
    }

#if defined(LIBRETRO_CORE)
    const auto* navigation_window = io.Ctx->NavWindow;
    frontend_input_captured.store(
        !layers.empty() && navigation_window && navigation_window->ID != dock_id &&
            (navigation_window->Flags & ImGuiWindowFlags_NoNav) == 0 &&
            (io.NavActive || io.WantCaptureKeyboard),
        std::memory_order_relaxed);
#endif

    return dockId;
}

void Render(const vk::CommandBuffer& cmdbuf, const vk::ImageView& image_view,
            const vk::Extent2D& extent, bool preserve_contents) {
    ImGui::Render();
    ImDrawData* draw_data = GetDrawData();
    // Font atlas uploads are recorded ahead of the render pass that samples them.
    Vulkan::UpdateTextures(*draw_data, cmdbuf);
    if (draw_data->CmdListsCount == 0) {
        return;
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "ImGui Render",
        });
    }

    vk::RenderingAttachmentInfo color_attachments[1]{
        {
            .imageView = image_view,
            .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
            .loadOp =
                preserve_contents ? vk::AttachmentLoadOp::eLoad : vk::AttachmentLoadOp::eClear,
            .storeOp = vk::AttachmentStoreOp::eStore,
        },
    };
    vk::RenderingInfo render_info{};
    render_info.renderArea = vk::Rect2D{
        .offset = {0, 0},
        .extent = extent,
    };
    render_info.layerCount = 1;
    render_info.colorAttachmentCount = 1;
    render_info.pColorAttachments = color_attachments;
    cmdbuf.beginRendering(render_info);
    Vulkan::RenderDrawData(*draw_data, cmdbuf);
    cmdbuf.endRendering();
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }
}

bool MustKeepDrawing() {
    return std::ranges::any_of(layers, [](Layer* layer) { return layer->ShouldKeepDrawing(); }) ||
           change_layers.size() > 1;
}

} // namespace Core

void Layer::AddLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(true, layer);
}

void Layer::RemoveLayer(Layer* layer) {
    std::scoped_lock lock{change_layers_mutex};
    change_layers.emplace_back(false, layer);
}

} // namespace ImGui
