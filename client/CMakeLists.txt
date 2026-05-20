# l2voice.dll — standalone voice client for L2 Essence 542.
# Separate project from l2ui/AutoLogin. Both DLLs can coexist in the
# client directory; they're injected independently via the Engine.dll
# IAT method (Themida on L2.exe is bypassed by NOT touching L2.exe).
#
# Build (from this directory):
#   cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
#   cmake --build build --config Release
#
# Output: build/Release/l2voice.dll

cmake_minimum_required(VERSION 3.20)
project(l2voice LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# L2 client is 32-bit. Refuse to configure as 64-bit so the user can't
# accidentally build a DLL the client can't load.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "l2voice must be built for Win32 (32-bit). Re-run cmake with -A Win32.")
endif()

include(FetchContent)

# ---- libopus (BSD) --------------------------------------------------
FetchContent_Declare(
    opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(opus)

# ---- miniaudio (MIT / Unlicense, header-only) -----------------------
FetchContent_Declare(
    miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.21
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniaudio)

# ---- stb_image (Public Domain) — PNG loader for the icon ------------
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

# ---- MinHook (BSD-2-Clause) — used by the D3D9 EndScene hook --------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

# ---- Dear ImGui (MIT) — in-game overlay -----------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(imgui)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC d3d9 d3dcompiler)

# ---- IXWebSocket (MIT) ----------------------------------------------
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5
    GIT_SHALLOW TRUE
)
set(USE_TLS      OFF CACHE BOOL "" FORCE)
set(USE_OPEN_SSL OFF CACHE BOOL "" FORCE)
set(USE_MBED_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB     OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# ---- the DLL --------------------------------------------------------
# Generate the resource script with an absolute path to the PNG so
# the RC compiler can find it at build time. Embedded into the DLL
# binary — no external file needed at runtime.
set(MIC_PNG_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../voice-recorder.png")
file(TO_NATIVE_PATH "${MIC_PNG_PATH}" MIC_PNG_PATH)
string(REPLACE "\\" "\\\\" MIC_PNG_PATH "${MIC_PNG_PATH}")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/voice/resources.rc.in
    ${CMAKE_CURRENT_BINARY_DIR}/l2voice_resources.rc
    @ONLY
)

add_library(l2voice SHARED
    dllmain.cpp
    voice/voice.cpp
    voice/audio_io.cpp
    voice/opus_codec.cpp
    voice/voice_network.cpp
    voice/overlay.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/l2voice_resources.rc
)

target_include_directories(l2voice PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/voice
    ${miniaudio_SOURCE_DIR}
    ${stb_SOURCE_DIR}
)

target_link_libraries(l2voice PRIVATE
    opus
    ixwebsocket
    minhook
    imgui
    ws2_32
    winmm
    iphlpapi
    d3d9
    dinput8
    dxguid
)

target_compile_definitions(l2voice PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(l2voice PROPERTIES
    OUTPUT_NAME "l2voice"
    PREFIX ""              # no "lib" prefix
    SUFFIX ".dll"
)

if(MSVC)
    target_compile_options(l2voice PRIVATE /W3 /MP /EHsc)
endif()
