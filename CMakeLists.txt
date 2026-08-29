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
