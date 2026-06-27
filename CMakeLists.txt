cmake_minimum_required(VERSION 3.24)

project(Pulse LANGUAGES CXX)

if (WIN32)
    enable_language(RC)
endif()

# Clean-sheet D3D12 + DXR engine. MSVC + Ninja, C++20.
# See docs/Plan_PULSE_engine_and_assets.txt.
#
# Layout:
#   pulse_engine  STATIC  src/Engine/** + src/Platform/**   (the engine core)
#   pulse         EXE      src/main.cpp + src/Game/**        (the game; ported in M0)
#   engine_smoke  EXE      tools/dev/engine_smoke.cpp        (engine bring-up driver)
#
# Vendored deps under third_party/ (provision with tools/Provision-ThirdParty.ps1):
#   Agility SDK (current D3D12 + redist debug layer), DXC, cgltf, stb_vorbis, texconv.

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(PULSE_TP ${CMAKE_SOURCE_DIR}/third_party)

# DirectX Agility SDK version: must match third_party/agility (D3D12_SDK_VERSION).
set(PULSE_AGILITY_SDK_VERSION 619)

# ------------------------------------------------------------------ engine lib
file(GLOB_RECURSE PULSE_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/Engine/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Platform/*.cpp"
)
add_library(pulse_engine STATIC ${PULSE_ENGINE_SOURCES})

target_compile_features(pulse_engine PUBLIC cxx_std_20)
target_include_directories(pulse_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${PULSE_TP}/agility/include    # MUST precede the Windows SDK d3d12.h
    ${PULSE_TP}/dxc/include
    ${PULSE_TP}/cgltf
    ${PULSE_TP}/stb
)
target_compile_definitions(pulse_engine PUBLIC
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    _CRT_SECURE_NO_WARNINGS
    PULSE_AGILITY_SDK_VERSION=${PULSE_AGILITY_SDK_VERSION}
)
# Dev configs compile shaders straight from the source tree for hot iteration; a
# shipped (Release) build reads assets/shaders next to the exe instead.
target_compile_definitions(pulse_engine PUBLIC
    $<$<NOT:$<CONFIG:Release>>:PULSE_SOURCE_SHADER_DIR="${CMAKE_SOURCE_DIR}/assets/shaders">)
target_link_libraries(pulse_engine PUBLIC
    user32 gdi32
    d3d12 dxgi dxguid
    ${PULSE_TP}/dxc/lib/x64/dxcompiler.lib
    ole32 avrt                                  # WASAPI audio backend
    dbghelp                                      # MiniDumpWriteDump (crash handler)
)

# --------------------------------------------------------------- compiler flags
if (MSVC)
    # /utf-8: treat sources as UTF-8 so any non-ASCII in a string literal is read
    # correctly (avoids codepage misinterpretation of the source bytes).
    target_compile_options(pulse_engine PUBLIC /W4 /permissive- /Zc:preprocessor /utf-8)
    target_compile_options(pulse_engine PUBLIC
        $<$<NOT:$<CONFIG:Debug>>:/O2;/fp:fast;/arch:AVX2>)
    # Debug layer on in Debug + RelWithDebInfo; GPU-based validation in Debug only.
    target_compile_definitions(pulse_engine PUBLIC
        $<$<CONFIG:Debug>:PULSE_GPU_VALIDATION=1>
        $<$<NOT:$<CONFIG:Release>>:PULSE_DEBUG_LAYER=1>)
endif()

# Stage the runtime DLLs (Agility Core + debug layer under D3D12/, dxc next to exe)
# for any executable that needs them at run time.
function(pulse_stage_runtime tgt)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:${tgt}>/D3D12
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PULSE_TP}/agility/bin/x64/D3D12Core.dll
            ${PULSE_TP}/agility/bin/x64/d3d12SDKLayers.dll
            $<TARGET_FILE_DIR:${tgt}>/D3D12/
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${PULSE_TP}/dxc/bin/x64/dxcompiler.dll
            ${PULSE_TP}/dxc/bin/x64/dxil.dll
            $<TARGET_FILE_DIR:${tgt}>/
        VERBATIM)
endfunction()

# ------------------------------------------------------------- game executable
# Ported in M0 (render/audio submission rewritten to SceneFrame). Until then this
# target will not compile; build engine_smoke for bring-up.
file(GLOB PULSE_GAME_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/Game/*.cpp"
)
if (WIN32)
    set(PULSE_ICON_RC_PATH "${CMAKE_SOURCE_DIR}/assets/icons/pulse.ico")
    configure_file("${CMAKE_SOURCE_DIR}/src/pulse.rc.in" "${CMAKE_BINARY_DIR}/pulse.rc" @ONLY)
    list(APPEND PULSE_GAME_SOURCES "${CMAKE_BINARY_DIR}/pulse.rc")
endif()
add_executable(pulse ${PULSE_GAME_SOURCES})
target_link_libraries(pulse PRIVATE pulse_engine)
# Ship the game on the WINDOWS subsystem so a normal launch (double-click) opens no console
# window. We keep int main() as the entry point (mainCRTStartup) and re-attach to the launching
# terminal at runtime when there is one (main.cpp::attachParentConsole), so the headless capture /
# CLI modes (--screenshot, --bot-test, --gpu-info, ...) still print. The dev tools below stay on
# the console subsystem - they are pure headless drivers whose output you always want.
if (WIN32)
    set_target_properties(pulse PROPERTIES WIN32_EXECUTABLE TRUE)
    if (MSVC)
        target_link_options(pulse PRIVATE /ENTRY:mainCRTStartup)
    endif()
endif()
pulse_stage_runtime(pulse)

# --------------------------------------------------------- dev executables
# engine_smoke: headless validation stages (clear/triangle/cube -> BMP).
if (EXISTS ${CMAKE_SOURCE_DIR}/tools/dev/engine_smoke.cpp)
    add_executable(engine_smoke ${CMAKE_SOURCE_DIR}/tools/dev/engine_smoke.cpp)
    target_link_libraries(engine_smoke PRIVATE pulse_engine)
    pulse_stage_runtime(engine_smoke)
endif()

# pulse_window: windowed live demo (spinning cube via the swapchain present path).
if (EXISTS ${CMAKE_SOURCE_DIR}/tools/dev/pulse_window.cpp)
    add_executable(pulse_window ${CMAKE_SOURCE_DIR}/tools/dev/pulse_window.cpp)
    target_include_directories(pulse_window PRIVATE ${CMAKE_SOURCE_DIR}/tools/dev)
    target_link_libraries(pulse_window PRIVATE pulse_engine)
    pulse_stage_runtime(pulse_window)
endif()

# wasteland_preview: headless arena preview (engine + Game/Wasteland only). Loads the kit's
# real PBR textures so the arena look can be checked without PulseGame.
if (EXISTS ${CMAKE_SOURCE_DIR}/tools/dev/wasteland_preview.cpp)
    add_executable(wasteland_preview
        ${CMAKE_SOURCE_DIR}/tools/dev/wasteland_preview.cpp
        ${CMAKE_SOURCE_DIR}/src/Game/Wasteland.cpp)
    target_link_libraries(wasteland_preview PRIVATE pulse_engine)
    pulse_stage_runtime(wasteland_preview)
endif()
