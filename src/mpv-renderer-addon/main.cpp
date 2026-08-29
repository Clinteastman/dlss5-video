// SPDX-License-Identifier: MIT

#include <windows.h>
#include <imgui.h>
#include <reshade.hpp>

#include "live_ngx.hpp"

#include <array>
#include <atomic>
#include <chrono>

namespace
{
std::atomic<unsigned long long> frameCount = 0;
std::atomic<unsigned long long> frozenFramePresents = 0;
std::atomic<double> frameMilliseconds = 0.0;
std::chrono::steady_clock::time_point previousPresent;
HMODULE addonModule = nullptr;
bool ngxTransportEnabled = true;
bool rendererEnabled = true;
bool useEstimatedDepth = true;
bool useEstimatedMotion = true;
bool reverseEstimatedDepth = false;
bool freezeFrame = false;
bool frozenFrameReady = false;
int debugView = 0;
float neuralStrength = 1.0f;
float depthHistory = 0.25f;
int depthDetail = 2;
int flowResolution = 2;
int flowPerformance = 0;
constexpr std::array<uint32_t, 3> depthSizes = {280, 392, 518};
constexpr std::array<uint32_t, 3> flowPercents = {25, 50, 100};
constexpr std::array<uint32_t, 3> flowPerformanceLevels = {20, 10, 5};
reshade::api::resource frameCopy = {};
reshade::api::device* frameCopyDevice = nullptr;
uint32_t frameCopyWidth = 0;
uint32_t frameCopyHeight = 0;
reshade::api::format frameCopyFormat = reshade::api::format::unknown;

void PrepareFrozenSource(
    reshade::api::command_list* commands,
    reshade::api::device* device,
    reshade::api::resource backBuffer,
    const reshade::api::resource_desc& sourceDesc);

void OnPresent(
    reshade::api::command_queue* queue,
    reshade::api::swapchain* swapchain,
    const reshade::api::rect*,
    const reshade::api::rect*,
    uint32_t,
    const reshade::api::rect*)
{
    const auto now = std::chrono::steady_clock::now();
    if (previousPresent.time_since_epoch().count() != 0)
    {
        frameMilliseconds.store(
            std::chrono::duration<double, std::milli>(now - previousPresent).count(),
            std::memory_order_relaxed);
    }
    previousPresent = now;
    const uint64_t presentIndex = frameCount.fetch_add(1, std::memory_order_relaxed);

    reshade::api::device* const device = swapchain->get_device();
    if (device->get_api() != reshade::api::device_api::d3d11) return;

    const reshade::api::resource backBuffer = swapchain->get_current_back_buffer();
    const reshade::api::resource_desc sourceDesc = device->get_resource_desc(backBuffer);

    // The generic present event runs before ReShade renders effects and its
    // overlay. Hold and process the clean video frame here so the UI remains
    // freshly rendered and interactive on every presentation.
    PrepareFrozenSource(
        queue->get_immediate_command_list(), device, backBuffer, sourceDesc);

    if (ngxTransportEnabled)
    {
        auto* const nativeDevice = reinterpret_cast<ID3D11Device*>(device->get_native());
        auto* const nativeBackBuffer = reinterpret_cast<ID3D11Texture2D*>(backBuffer.handle);
        live_ngx::evaluate_transport(
            nativeDevice, nativeBackBuffer, addonModule,
            sourceDesc.texture.width, sourceDesc.texture.height,
            rendererEnabled && debugView == 0,
            debugView, presentIndex, freezeFrame,
            useEstimatedDepth, useEstimatedMotion, reverseEstimatedDepth);
        live_ngx::set_guide_quality(
            depthSizes[depthDetail],
            flowPercents[flowResolution],
            flowPerformanceLevels[flowPerformance]);
    }
}

void DestroyFrameCopy()
{
    if (frameCopy.handle != 0 && frameCopyDevice != nullptr)
        frameCopyDevice->destroy_resource(frameCopy);
    frameCopy = {};
    frameCopyDevice = nullptr;
    frameCopyWidth = 0;
    frameCopyHeight = 0;
    frameCopyFormat = reshade::api::format::unknown;
    frozenFrameReady = false;
}

bool EnsureFrameCopy(
    reshade::api::device* device,
    const reshade::api::resource_desc& sourceDesc)
{
    if (frameCopy.handle == 0 || frameCopyDevice != device ||
        frameCopyWidth != sourceDesc.texture.width ||
        frameCopyHeight != sourceDesc.texture.height ||
        frameCopyFormat != sourceDesc.texture.format)
    {
        DestroyFrameCopy();
        reshade::api::resource_desc copyDesc = sourceDesc;
        copyDesc.usage = reshade::api::resource_usage::copy_source |
                         reshade::api::resource_usage::copy_dest;
        if (!device->create_resource(
                copyDesc, nullptr, reshade::api::resource_usage::copy_dest, &frameCopy))
        {
            reshade::log::message(
                reshade::log::level::error,
                "DLSS 5 Video Renderer could not create its frozen-frame texture.");
            return false;
        }
        frameCopyDevice = device;
        frameCopyWidth = sourceDesc.texture.width;
        frameCopyHeight = sourceDesc.texture.height;
        frameCopyFormat = sourceDesc.texture.format;
    }
    return true;
}

void PrepareFrozenSource(
    reshade::api::command_list* commands,
    reshade::api::device* device,
    reshade::api::resource backBuffer,
    const reshade::api::resource_desc& sourceDesc)
{
    if (!freezeFrame)
    {
        frozenFrameReady = false;
        return;
    }

    if (!EnsureFrameCopy(device, sourceDesc)) return;

    if (!frozenFrameReady)
    {
        commands->barrier(
            backBuffer, reshade::api::resource_usage::present,
            reshade::api::resource_usage::copy_source);
        commands->copy_resource(backBuffer, frameCopy);
        commands->barrier(
            backBuffer, reshade::api::resource_usage::copy_source,
            reshade::api::resource_usage::present);
        frozenFrameReady = true;
        return;
    }

    commands->barrier(
        frameCopy, reshade::api::resource_usage::copy_dest,
        reshade::api::resource_usage::copy_source);
    commands->barrier(
        backBuffer, reshade::api::resource_usage::present,
        reshade::api::resource_usage::copy_dest);
    commands->copy_resource(frameCopy, backBuffer);
    commands->barrier(
        frameCopy, reshade::api::resource_usage::copy_source,
        reshade::api::resource_usage::copy_dest);
    commands->barrier(
        backBuffer, reshade::api::resource_usage::copy_dest,
        reshade::api::resource_usage::present);
    frozenFramePresents.fetch_add(1, std::memory_order_relaxed);
}

void OnDestroyRuntime(reshade::api::effect_runtime*)
{
    DestroyFrameCopy();
}

void DrawSettings(reshade::api::effect_runtime*)
{
    ImGui::TextUnformatted("Live mpv renderer bridge");
    ImGui::Separator();
    ImGui::Checkbox("Run NGX transport", &ngxTransportEnabled);
    ImGui::Checkbox("Enable Neural Rendering", &rendererEnabled);
    ImGui::Checkbox("Use estimated depth", &useEstimatedDepth);
    ImGui::Checkbox("Use estimated motion", &useEstimatedMotion);
    ImGui::Checkbox("Reverse estimated depth values", &reverseEstimatedDepth);
    ImGui::Checkbox("Freeze displayed frame", &freezeFrame);

    constexpr std::array<const char*, 4> views = {
        "Final result", "Original / bypass", "Estimated depth", "Optical flow"
    };
    ImGui::Combo("View", &debugView, views.data(), static_cast<int>(views.size()));
    ImGui::SliderFloat("Neural strength", &neuralStrength, 0.0f, 1.5f, "%.2f");
    ImGui::SliderFloat("Depth history", &depthHistory, 0.0f, 0.9f, "%.2f");

    ImGui::Separator();
    ImGui::TextUnformatted("Live guide quality");
    constexpr std::array<const char*, 3> depthOptions = {
        "Fast (280)", "Balanced (392)", "High (518)"
    };
    constexpr std::array<const char*, 3> flowResolutionOptions = {
        "Quarter resolution", "Half resolution", "Full resolution"
    };
    constexpr std::array<const char*, 3> flowPerformanceOptions = {
        "Fast", "Balanced", "Quality"
    };
    ImGui::Combo(
        "Depth resolution", &depthDetail,
        depthOptions.data(), static_cast<int>(depthOptions.size()));
    ImGui::Combo(
        "Optical-flow resolution", &flowResolution,
        flowResolutionOptions.data(),
        static_cast<int>(flowResolutionOptions.size()));
    ImGui::Combo(
        "Optical-flow mode", &flowPerformance,
        flowPerformanceOptions.data(),
        static_cast<int>(flowPerformanceOptions.size()));
    ImGui::TextDisabled(
        "Quality mode may stall when full-resolution NVOF and NGX compete.");

    const double interval = frameMilliseconds.load(std::memory_order_relaxed);
    ImGui::Separator();
    ImGui::Text("Presented frames: %llu", frameCount.load(std::memory_order_relaxed));
    ImGui::Text("Frozen-frame presents: %llu", frozenFramePresents.load(std::memory_order_relaxed));
    ImGui::Text("NGX evaluations: %llu", live_ngx::evaluation_count());
    ImGui::Text("Guide frame: %u / %u",
        live_ngx::guide_frame_index(), live_ngx::guide_frame_count());
    ImGui::TextWrapped("Guides: %s", live_ngx::guide_status());
    ImGui::TextWrapped("NGX guide binding: %s", live_ngx::guide_binding_status());
    ImGui::TextWrapped("Status: %s", live_ngx::status());
    const double guideMilliseconds = live_ngx::guide_processing_milliseconds();
    if (guideMilliseconds > 0.0)
        ImGui::Text(
            "Guide processing: %.1f ms (%.1f fps)",
            guideMilliseconds, 1000.0 / guideMilliseconds);
    const uint32_t flowWidth = live_ngx::active_flow_width();
    const uint32_t flowHeight = live_ngx::active_flow_height();
    if (flowWidth != 0 && flowHeight != 0)
        ImGui::Text("Active optical flow: %ux%u", flowWidth, flowHeight);
    if (interval > 0.0)
        ImGui::Text("Present interval: %.2f ms (%.1f fps)", interval, 1000.0 / interval);
    ImGui::TextDisabled("Freeze holds the source frame while mpv keeps the UI alive.");
}
}

extern "C" __declspec(dllexport) const char* NAME = "DLSS 5 Video Renderer";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "Live Neural Rendering, guide visualization, and diagnostics for mpv.";

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        addonModule = module;
        if (!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::present>(OnPresent);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
        reshade::register_overlay(nullptr, DrawSettings);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        reshade::unregister_event<reshade::addon_event::present>(OnPresent);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyRuntime);
        reshade::unregister_overlay(nullptr, DrawSettings);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
