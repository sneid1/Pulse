cmake_minimum_required(VERSION 3.24)

project(Pulse LANGUAGES CXX)

# Clean-sheet D3D12 engine. MSVC + Ninja, C++20. See docs/Plan — PULSE engine and assets.txt.
# NOTE: this is an M0 scaffold. It will NOT link until the engine core exists
# (src/Engine/RHI, RenderGraph, Render, Assets, Audio) and PulseGame's render/audio
# submission layer + main.cpp are rewritten against the new engine. See SEED_NOTES.md.

# Pick up sources as M0 adds them (RHI/RenderGraph/Render/... appear under src/).
file(GLOB_RECURSE PULSE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

add_executable(pulse ${PULSE_SOURCES})

target_compile_features(pulse PRIVATE cxx_std_20)
target_include_directories(pulse PRIVATE src)
target_compile_definitions(pulse PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# D3D12 + DXR toolchain (Agility SDK headers/libs provisioned under third_party/ in M0).
target_link_libraries(pulse PRIVATE
    user32 gdi32
    d3d12 dxgi dxguid dxcompiler
    # WASAPI audio backend:
    ole32 avrt
)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if (MSVC)
    target_compile_options(pulse PRIVATE /W4 /permissive- /Zc:preprocessor)
    # /arch:AVX2 is the portable replacement for the prototype's -march=native.
    target_compile_options(pulse PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2 /fp:fast /arch:AVX2>)
    # D3D12 debug layer + GPU-based validation live behind this in debug/CI.
    target_compile_definitions(pulse PRIVATE $<$<CONFIG:Debug>:PULSE_GPU_VALIDATION=1>)
endif()
