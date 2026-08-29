// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include <d3d11.h>

#include <cstdint>

namespace live_ngx
{
bool evaluate_transport(
    ID3D11Device* hostDevice,
    ID3D11Texture2D* backBuffer,
    HMODULE addonModule,
    uint32_t width,
    uint32_t height,
    bool applyOutput,
    int debugView,
    uint64_t presentIndex,
    bool holdGuide,
    bool useEstimatedDepth,
    bool useEstimatedMotion,
    bool reverseEstimatedDepth);
uint64_t evaluation_count();
uint32_t guide_frame_index();
uint32_t guide_frame_count();
void set_guide_quality(
    uint32_t depthSize,
    uint32_t flowPercent,
    uint32_t flowPerformance);
double guide_processing_milliseconds();
uint32_t active_flow_width();
uint32_t active_flow_height();
const char* guide_status();
const char* guide_binding_status();
const char* status();
}
