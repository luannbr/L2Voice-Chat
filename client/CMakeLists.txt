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

# ---- rnnoise (Xiph, BSD) — neural noise suppression -----------------
# Upstream xiph/rnnoise master moved to autotools/meson-generated config
# headers and broke MSVC. cpuimage/rnnoise is a long-standing, MSVC-clean
# mirror that keeps the same Xiph BSD license, embeds the trained weights
# in rnn_data.c, and matches the upstream public API (rnnoise_create /
# rnnoise_destroy / rnnoise_process_frame in rnnoise.h).
FetchContent_Declare(
    rnnoise
    GIT_REPOSITORY https://github.com/cpuimage/rnnoise.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(rnnoise)
if(NOT rnnoise_POPULATED)
    FetchContent_Populate(rnnoise)
endif()
# cpuimage layout: rnnoise.h in include/, .c sources in src/. Uses
# stb_fft.h (header-only) instead of kiss_fft.c, and the trained model is
# baked into rnn_data.c — no rnn_reader.c needed.
add_library(rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/celt_lpc.c
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/pitch.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/rnn_data.c
)
target_include_directories(rnnoise PUBLIC
    ${rnnoise_SOURCE_DIR}/include
)
target_include_directories(rnnoise PRIVATE
    ${rnnoise_SOURCE_DIR}/src
)
# rnnoise is pure C — suppress MSVC's chatter about implicit conversions.
if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0)
endif()

# ---- Speex DSP (Xiph, BSD) — acoustic echo cancellation -------------
# We only use the AEC subset (mdf.c). NS/AGC are handled by rnnoise +
# our Apm. xiph/speexdsp has no upstream CMakeLists, so we list the
# minimal AEC-only file set and provide a tiny config.h. Pure float
# build (FLOATING_POINT) with the bundled KISS FFT backend.
FetchContent_Declare(
    speexdsp
    GIT_REPOSITORY https://github.com/xiph/speexdsp.git
    GIT_TAG SpeexDSP-1.2.1
    GIT_SHALLOW TRUE
)
FetchContent_GetProperties(speexdsp)
if(NOT speexdsp_POPULATED)
    FetchContent_Populate(speexdsp)
endif()
# Minimal config.h — speexdsp uses HAVE_CONFIG_H gating internally.
# Without it the source defaults are fine for a basic float build, so
# we deliberately don't define HAVE_CONFIG_H — speexdsp falls back to
# its built-in defaults via #ifdef HAVE_CONFIG_H guards.
add_library(speexdsp STATIC
    ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fftr.c
)
target_include_directories(speexdsp PUBLIC
    ${speexdsp_SOURCE_DIR}/include
)
target_include_directories(speexdsp PRIVATE
    ${speexdsp_SOURCE_DIR}/libspeexdsp
)
target_compile_definitions(speexdsp PRIVATE
    FLOATING_POINT     # use float (not fixed-point) math
    USE_KISS_FFT       # bundle the kiss_fft backend
    EXPORT=            # speex's "extern" decorator — empty for static lib
    RANDOM_PREFIX=speex_rnd
)
if(MSVC)
    target_compile_options(speexdsp PRIVATE /W0)
endif()

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
# VOICE_L2_THEME (default OFF): when ON, the overlay uses native L2UI
# textures for buttons/frames/tabs. Those textures are NCSoft IP and
# NOT distributed with this repo — users who own an L2 client can
# supply their own `l2ui_assets/` folder at the repo root. When OFF
# the overlay falls back to the ImGui default look and only the
# microphone PNGs (originally authored / public-domain icons) are
# embedded. This is the supported open-source path.
option(VOICE_L2_THEME "Bundle NCSoft L2UI textures (requires user-supplied l2ui_assets/)" OFF)

# Generate the resource script with absolute paths to the PNGs so
# the RC compiler can find them at build time. PNGs listed here are
# embedded into the DLL binary — no external file needed at runtime.
function(_resolve_resource_png var_name rel_path)
    set(p "${CMAKE_CURRENT_SOURCE_DIR}/${rel_path}")
    file(TO_NATIVE_PATH "${p}" p)
    string(REPLACE "\\" "\\\\" p "${p}")
    set(${var_name} "${p}" PARENT_SCOPE)
endfunction()
# Microphone-state icons — always embedded.
_resolve_resource_png(MIC_PNG_PATH          "../voice-recorder.png")
_resolve_resource_png(MIC_SPEAKING_PNG_PATH "../microfone_falando.png")
_resolve_resource_png(MIC_MUTED_PNG_PATH    "../microfone_mudo.png")
_resolve_resource_png(MIC_BLOCKED_PNG_PATH  "../microfone_block.png")

if(VOICE_L2_THEME)
    message(STATUS "VOICE_L2_THEME=ON — expecting l2ui_assets/ at repo root.")
    _resolve_resource_png(L2UI_BTN_BIG_PATH           "../l2ui_assets/L2UI_CH3_Button_bigbutton3.png")
    _resolve_resource_png(L2UI_BTN_BIG_OVER_PATH      "../l2ui_assets/L2UI_CH3_Button_bigbutton3_over.png")
    _resolve_resource_png(L2UI_BTN_BIG_DOWN_PATH      "../l2ui_assets/L2UI_CH3_Button_bigbutton3_down.png")
    _resolve_resource_png(L2UI_BTN_SMALL_PATH         "../l2ui_assets/L2UI_CH3_Button_smallbutton1.png")
    _resolve_resource_png(L2UI_BTN_SMALL_OVER_PATH    "../l2ui_assets/L2UI_CH3_Button_smallbutton1_over.png")
    _resolve_resource_png(L2UI_BTN_SMALL_DOWN_PATH    "../l2ui_assets/L2UI_CH3_Button_smallbutton1_down.png")
    _resolve_resource_png(L2UI_FRAME_MINI_PATH        "../l2ui_assets/L2UI_CH3_FRAMECTRL_FrameMiniBtn.png")
    _resolve_resource_png(L2UI_FRAME_MINI_OVER_PATH   "../l2ui_assets/L2UI_CH3_FRAMECTRL_frameminibtn_over.png")
    _resolve_resource_png(L2UI_FRAME_MINI_DOWN_PATH   "../l2ui_assets/L2UI_CH3_FRAMECTRL_FrameMiniBtn_down.png")
    _resolve_resource_png(L2UI_FRAME_CLOSE_PATH       "../l2ui_assets/L2UI_CH3_FRAMECTRL_FrameCloseBtn.png")
    _resolve_resource_png(L2UI_FRAME_CLOSE_OVER_PATH  "../l2ui_assets/L2UI_CH3_FRAMECTRL_frameclosebtn_over.png")
    _resolve_resource_png(L2UI_FRAME_CLOSE_DOWN_PATH  "../l2ui_assets/L2UI_CH3_FRAMECTRL_FrameCloseBtn_down.png")
    _resolve_resource_png(L2UI_TAB_SELECTED_PATH         "../l2ui_assets/L2UI_NewTex_HennaWnd_TabBtn_Selected.png")
    _resolve_resource_png(L2UI_TAB_UNSELECTED_PATH       "../l2ui_assets/L2UI_NewTex_HennaWnd_TabBtn_Unselected.png")
    _resolve_resource_png(L2UI_TAB_UNSELECTED_OVER_PATH  "../l2ui_assets/L2UI_NewTex_HennaWnd_TabBtn_Unselected_Over.png")
    _resolve_resource_png(L2UI_WND_BG_PATH               "../l2ui_assets/L2UI_CH3_NpcWnd_npc1_back.png")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/voice/resources.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/l2voice_resources.rc
        @ONLY
    )
else()
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/voice/resources_minimal.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/l2voice_resources.rc
        @ONLY
    )
endif()

add_library(l2voice SHARED
    dllmain.cpp
    voice/voice.cpp
    voice/audio_io.cpp
    voice/opus_codec.cpp
    voice/voice_network.cpp
    voice/overlay.cpp
    voice/apm.cpp
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
    rnnoise
    speexdsp
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
