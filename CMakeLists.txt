cmake_minimum_required(VERSION 3.24)

project(dlss5_video_lab VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "DLSS 5 Video Lab currently supports Windows only")
endif()

add_executable(ngx-capability-probe
    src/ngx-capability-probe/main.cpp
)

target_compile_features(ngx-capability-probe PRIVATE cxx_std_20)
target_compile_definitions(ngx-capability-probe PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)
target_compile_options(ngx-capability-probe PRIVATE /W4 /permissive- /EHsc)
target_link_libraries(ngx-capability-probe PRIVATE d3d12 dxgi)

set(RESHADE_SOURCE_DIR "" CACHE PATH "Path to a ReShade source checkout")
if(RESHADE_SOURCE_DIR)
    if(NOT EXISTS "${RESHADE_SOURCE_DIR}/include/reshade.hpp")
        message(FATAL_ERROR "RESHADE_SOURCE_DIR does not contain include/reshade.hpp")
    endif()
    if(NOT EXISTS "${RESHADE_SOURCE_DIR}/deps/imgui/imgui.h")
        message(FATAL_ERROR "Initialize ReShade's deps/imgui submodule first")
    endif()

    add_library(dlss5-video-renderer SHARED
        src/mpv-renderer-addon/main.cpp
        src/mpv-renderer-addon/live_ngx.cpp
    )
    target_compile_features(dlss5-video-renderer PRIVATE cxx_std_20)
    target_compile_definitions(dlss5-video-renderer PRIVATE
        RESHADE_ADDON=2
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_compile_options(dlss5-video-renderer PRIVATE /W4 /permissive- /EHsc)
    target_include_directories(dlss5-video-renderer PRIVATE
        "${RESHADE_SOURCE_DIR}/include"
        "${RESHADE_SOURCE_DIR}/deps/imgui"
    )
    set_target_properties(dlss5-video-renderer PROPERTIES
        PREFIX ""
        SUFFIX ".addon64"
    )
    target_link_libraries(dlss5-video-renderer PRIVATE d3d11 d3d12 dxgi)
endif()
