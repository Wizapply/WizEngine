cmake_minimum_required(VERSION 3.21)

# MSVC_RUNTIME_LIBRARY (rather than raw /MT|/MD flags) selects the CRT.
cmake_policy(SET CMP0091 NEW)

project(WizEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Filament (prebuilt, fetched automatically)
# By default the matching prebuilt release is downloaded and used - no manual
# download and no source build. To use a local copy instead, configure with
#   -DFILAMENT_ROOT=/path/to/filament
# and the download is skipped. Override the version with -DFILAMENT_VERSION=...
# ---------------------------------------------------------------------------
set(FILAMENT_VERSION "1.74.0" CACHE STRING "Filament prebuilt release to fetch")
set(FILAMENT_ROOT "" CACHE PATH "Use a local Filament release instead of fetching")

# The presets point FILAMENT_ROOT into third_parties/ so a local copy is used
# when present - but a fresh clone has no third_parties/, and that must not
# break the build: fall back to fetching instead.
if(FILAMENT_ROOT AND NOT EXISTS "${FILAMENT_ROOT}")
    message(STATUS
        "WizEngine: FILAMENT_ROOT=${FILAMENT_ROOT} does not exist - "
        "fetching the prebuilt release instead")
    set(FILAMENT_ROOT "")
endif()

if(NOT FILAMENT_ROOT)
    include(FetchContent)
    if(WIN32)
        set(_filament_os windows)
    elseif(APPLE)
        set(_filament_os mac)
    else()
        set(_filament_os linux)
    endif()
    FetchContent_Declare(filament_prebuilt
        URL https://github.com/google/filament/releases/download/v${FILAMENT_VERSION}/filament-v${FILAMENT_VERSION}-${_filament_os}.tgz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(filament_prebuilt)

    # The archive extracts either directly (include/ at top) or under filament/.
    if(EXISTS "${filament_prebuilt_SOURCE_DIR}/include")
        set(FILAMENT_ROOT "${filament_prebuilt_SOURCE_DIR}")
    elseif(EXISTS "${filament_prebuilt_SOURCE_DIR}/filament/include")
        set(FILAMENT_ROOT "${filament_prebuilt_SOURCE_DIR}/filament")
    else()
        message(FATAL_ERROR "Unexpected Filament layout under ${filament_prebuilt_SOURCE_DIR}")
    endif()
    message(STATUS "WizEngine: using fetched Filament ${FILAMENT_VERSION} -> ${FILAMENT_ROOT}")
else()
    message(STATUS "WizEngine: using local Filament -> ${FILAMENT_ROOT}")
endif()

if(WIN32)
    # FILAMENT_LIBDIR is chosen further down, once the MSVC runtime is known:
    # Filament ships one set of libs per CRT (md/mdd for /MD, mt/mtd for /MT).
    set(MATC "${FILAMENT_ROOT}/bin/matc.exe")
else()
    set(FILAMENT_LIBDIR "${FILAMENT_ROOT}/lib/x86_64")
    set(MATC "${FILAMENT_ROOT}/bin/matc")
endif()

# ---------------------------------------------------------------------------
# Chrono (built from source and installed)
# This Chrono uses the variable-based CMake interface (no Chrono::main target):
# CHRONO_INCLUDE_DIRS / CHRONO_LIBRARIES / CHRONO_CXX_FLAGS / CHRONO_LINKER_FLAGS.
#
# Easiest setup: set CHRONO_ROOT to the install prefix (the folder you passed to
# CMAKE_INSTALL_PREFIX when installing Chrono). It shows up in cmake-gui and we
# work out Chrono_DIR from it. Setting Chrono_DIR directly also still works.
# ---------------------------------------------------------------------------
set(CHRONO_ROOT "" CACHE PATH
    "Chrono install prefix (the folder containing ChronoConfig.cmake, usually under cmake/)")

if(CHRONO_ROOT AND NOT Chrono_DIR)
    foreach(_chrono_candidate
            "${CHRONO_ROOT}/cmake"
            "${CHRONO_ROOT}/lib/cmake/Chrono"
            "${CHRONO_ROOT}/lib64/cmake/Chrono"
            "${CHRONO_ROOT}")
        if(EXISTS "${_chrono_candidate}/ChronoConfig.cmake")
            set(Chrono_DIR "${_chrono_candidate}" CACHE PATH "Chrono config dir" FORCE)
            message(STATUS "WizEngine: found Chrono config -> ${Chrono_DIR}")
            break()
        endif()
    endforeach()
    if(NOT Chrono_DIR)
        message(WARNING
            "CHRONO_ROOT=${CHRONO_ROOT} does not contain ChronoConfig.cmake. "
            "Search for that file and set Chrono_DIR to the folder holding it.")
    endif()
endif()

# Chrono::Multicore (OpenMP-parallel solver + collision) instead of the serial
# core solver. Requires a Chrono built with CH_ENABLE_MODULE_MULTICORE=ON
# (which in turn needs Blaze and Thrust). Off by default so the project still
# configures against a plain Chrono install.
option(WIZ_USE_MULTICORE
       "Use Chrono::Multicore (parallel) instead of the serial Chrono solver" OFF)

if(WIZ_USE_MULTICORE)
    find_package(Chrono COMPONENTS Multicore CONFIG REQUIRED)
    message(STATUS "WizEngine: physics engine = Chrono::Multicore (parallel)")
else()
    find_package(Chrono CONFIG REQUIRED)
    message(STATUS "WizEngine: physics engine = Chrono core (serial)")
endif()

# ---------------------------------------------------------------------------
# GStreamer (via pkg-config)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0
    gstreamer-video-1.0)  # video: force-key-unit events

# ---------------------------------------------------------------------------
# Compile the material with matc -> shaded.filamat (next to the executable)
# ---------------------------------------------------------------------------
set(MAT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/shaded.mat)
# Everything the program loads at runtime is gathered under one folder, so the
# executable has a single place to look and the whole set can be copied around
# together.
set(ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${ASSET_DIR})
# エディタが保存するシーン（assets/scenes/*.json）の置き場。実行時にも
# 作られるが、初回起動前から一覧に見えるように先に用意しておく。
file(MAKE_DIRECTORY ${ASSET_DIR}/scenes)

set(MAT_OUT ${ASSET_DIR}/shaded.filamat)
add_custom_command(
    OUTPUT ${MAT_OUT}
    COMMAND ${MATC} -a all -p desktop -o ${MAT_OUT} ${MAT_SRC}
    DEPENDS ${MAT_SRC}
    COMMENT "Compiling Filament material (matc, all shader languages)")

set(MAT_SRC2 ${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/ground_lit.mat)
set(MAT_OUT2 ${ASSET_DIR}/ground_lit.filamat)
add_custom_command(
    OUTPUT ${MAT_OUT2}
    COMMAND ${MATC} -a all -p desktop -o ${MAT_OUT2} ${MAT_SRC2}
    DEPENDS ${MAT_SRC2}
    COMMENT "Compiling Filament ground material (matc)")

set(MAT_SRC3 ${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/line.mat)
set(MAT_OUT3 ${ASSET_DIR}/line.filamat)
add_custom_command(
    OUTPUT ${MAT_OUT3}
    COMMAND ${MATC} -a all -p desktop -o ${MAT_OUT3} ${MAT_SRC3}
    DEPENDS ${MAT_SRC3}
    COMMENT "Compiling Filament line material (matc)")

add_custom_target(materials ALL DEPENDS ${MAT_OUT} ${MAT_OUT2} ${MAT_OUT3})

# The ground texture lives in assets/textures/ and is carried to the runtime
# dir by the wholesale assets/ copy below (sceneassets). Drop your own PNG
# there (same name) to change the floor.
# Any .hdr in assets/ is used as-is at runtime; nothing to convert.

# Environment maps and any other model/texture the scene names: copied as they
# are, since the conversion to a cubemap now happens at load time on the GPU.
set(ASSET_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets)
if(EXISTS ${ASSET_SRC_DIR})
    file(GLOB SCENE_ASSETS ${ASSET_SRC_DIR}/*)
    add_custom_target(sceneassets ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${ASSET_SRC_DIR} ${ASSET_DIR}
        DEPENDS ${SCENE_ASSETS}
        COMMENT "Copying scene assets (models, environment maps)")
else()
    message(STATUS
        "WizEngine: no assets/ folder in the source tree - put .hdr and .glb "
        "files there and they will be copied next to the executable")
endif()

# Web UI: the browser page and its assets are served from disk at runtime, so
# they are copied next to the binary. Editing a file under web/ and reloading
# the browser is enough - no rebuild needed.
set(WEB_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/web)
set(WEB_OUT_DIR ${ASSET_DIR}/web)
file(GLOB WEB_FILES ${WEB_SRC_DIR}/*)
add_custom_target(webui ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${WEB_SRC_DIR} ${WEB_OUT_DIR}
    DEPENDS ${WEB_FILES}
    COMMENT "Copying web UI")

# ---------------------------------------------------------------------------
# Header-only dependencies: cpp-httplib (control server) and nlohmann/json
# (browser input). Downloaded as single headers into the build dir - a few
# hundred KB each, no git clone. (Cloning nlohmann/json pulls a huge repo and
# can stall configure for many minutes, so we avoid it.)
# ---------------------------------------------------------------------------
set(DEPS_INC "${CMAKE_BINARY_DIR}/_deps/single-include")
file(MAKE_DIRECTORY "${DEPS_INC}/nlohmann")

function(wiz_download_header url dest)
    if(EXISTS "${dest}")
        return()
    endif()
    message(STATUS "WizEngine: downloading ${url}")
    file(DOWNLOAD "${url}" "${dest}" TLS_VERIFY ON STATUS _dl_status)
    list(GET _dl_status 0 _dl_code)
    list(GET _dl_status 1 _dl_msg)
    if(NOT _dl_code EQUAL 0)
        file(REMOVE "${dest}")
        message(FATAL_ERROR "Download failed (${_dl_msg}): ${url}")
    endif()
endfunction()

wiz_download_header(
    "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h"
    "${DEPS_INC}/httplib.h")
wiz_download_header(
    "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
    "${DEPS_INC}/nlohmann/json.hpp")
# cgltf: parses glTF/GLB on the CPU for collision geometry (the renderer's
# gltfio streams vertices straight to the GPU, so physics reads the file
# itself - with the same parser gltfio uses internally).
wiz_download_header(
    "https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.14/cgltf.h"
    "${DEPS_INC}/cgltf.h")
# stb_image: decodes PNG/JPEG/TGA/BMP for the ground texture.
wiz_download_header(
    "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
    "${DEPS_INC}/stb_image.h")

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(wizengine
    src/main.cpp
    src/PhysicsWorld.cpp
    src/MeshCollision.cpp
    src/Renderer.cpp
    src/EnvironmentLoader.cpp
    src/scene.cpp
    src/VideoStreamer.cpp
    src/WebRtcStreamer.cpp
    src/HttpServer.cpp
    src/ImageLoader.cpp
    src/Log.cpp
    src/Versions.cpp
    src/CameraObject.cpp
    src/LightObject.cpp
    src/GltfLoader.cpp
    src/PortScan.cpp
    src/AssetError.cpp
    src/EditorState.cpp
    src/EditorComponent.cpp
    src/GizmoComponent.cpp
    src/CpuAffinity.cpp)
# Version pins surfaced in the browser's About section (src/Versions.cpp).
# WIZ_CGLTF_VERSION must match the wiz_download_header URL above.
target_compile_definitions(wizengine PRIVATE
    WIZ_FILAMENT_VERSION="${FILAMENT_VERSION}"
    WIZ_CGLTF_VERSION="1.14")

add_dependencies(wizengine materials)

# Filament's public headers use designated initializers. clang/gcc accept these
# in C++17, but MSVC requires C++20 for them - so compile the translation units
# that include Filament headers as C++20. The rest of the project stays C++17
# (CMAKE_CXX_STANDARD above).
if(MSVC)
    set_source_files_properties(src/Renderer.cpp src/GltfLoader.cpp
        src/EnvironmentLoader.cpp
        PROPERTIES COMPILE_OPTIONS "/std:c++20")

    # Sources (and string literals) are UTF-8. Without this MSVC reads them as
    # the system code page - on a Japanese Windows that mangles the Japanese
    # text embedded in web_client.h and even eats the '<' of following tags,
    # which breaks the served HTML.
    target_compile_options(wizengine PRIVATE /utf-8)
endif()

if(WIZ_USE_MULTICORE)
    target_compile_definitions(wizengine PRIVATE WIZ_USE_MULTICORE)
endif()

target_include_directories(wizengine PRIVATE
    ${FILAMENT_ROOT}/include
    ${CHRONO_INCLUDE_DIRS}
    ${DEPS_INC}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Chrono requires its own compile/link flags (AVX, EH handling) - and it also
# reports the CRT it was built against (/MT or /MD). Mixing CRTs across object
# files gives duplicate STL symbols (LNK2005 on std::ios_base::good and
# friends) or missing ones (unresolved type_info::`vftable'), so the CRT is set
# once, for the whole target, via MSVC_RUNTIME_LIBRARY.
#
# The default is auto-detected from Chrono's flags; if Chrono's flags do not
# mention it, override explicitly:
#   cmake -B build -DWIZ_MSVC_STATIC_RUNTIME=ON     (Chrono built with /MT)
#   cmake -B build -DWIZ_MSVC_STATIC_RUNTIME=OFF    (Chrono built with /MD)
if(MSVC)
    message(STATUS "WizEngine: CHRONO_CXX_FLAGS    = ${CHRONO_CXX_FLAGS}")
    message(STATUS "WizEngine: CHRONO_LINKER_FLAGS = ${CHRONO_LINKER_FLAGS}")

    string(FIND "${CHRONO_CXX_FLAGS}" "/MT" _chrono_static_crt)
    if(_chrono_static_crt GREATER -1)
        set(_wiz_static_default ON)
    else()
        set(_wiz_static_default OFF)
    endif()
    set(WIZ_MSVC_STATIC_RUNTIME ${_wiz_static_default} CACHE BOOL
        "Link the static MSVC runtime (/MT). Must match how Chrono was built.")

    if(WIZ_MSVC_STATIC_RUNTIME)
        message(STATUS "WizEngine: using the STATIC MSVC runtime (/MT)")
        set_property(TARGET wizengine PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    else()
        message(STATUS "WizEngine: using the DYNAMIC MSVC runtime (/MD)")
        set_property(TARGET wizengine PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()
endif()

# Filament must be linked against the same CRT. Its Windows release ships
# lib/x86_64/{md,mdd,mt,mtd}; pick the set that matches.
if(WIN32)
    if(WIZ_MSVC_STATIC_RUNTIME)
        set(FILAMENT_LIBDIR "${FILAMENT_ROOT}/lib/x86_64/$<IF:$<CONFIG:Debug>,mtd,mt>")
    else()
        set(FILAMENT_LIBDIR "${FILAMENT_ROOT}/lib/x86_64/$<IF:$<CONFIG:Debug>,mdd,md>")
    endif()
    message(STATUS "WizEngine: Filament libs -> ${FILAMENT_LIBDIR}")

    # GStreamer's official Windows builds use the dynamic CRT, so /MD is the
    # combination that works with every dependency here. If Chrono was built
    # with /MT, rebuild it with USE_MSVC_STATIC_RUNTIME=OFF rather than forcing
    # /MT on this target.
    if(WIZ_MSVC_STATIC_RUNTIME)
        message(WARNING
            "Building against the STATIC MSVC runtime (/MT). GStreamer expects "
            "/MD; if the link fails with duplicate or unresolved CRT symbols, "
            "rebuild Chrono with USE_MSVC_STATIC_RUNTIME=OFF and configure with "
            "-DWIZ_MSVC_STATIC_RUNTIME=OFF.")
    endif()
endif()

if(CHRONO_CXX_FLAGS)
    # Drop /MT, /MTd, /MD, /MDd - handled by MSVC_RUNTIME_LIBRARY above.
    string(REGEX REPLACE "/M[TD]d?" " " _chrono_cxx_flags "${CHRONO_CXX_FLAGS}")
    separate_arguments(_chrono_cxx_flags NATIVE_COMMAND "${_chrono_cxx_flags}")
    target_compile_options(wizengine PRIVATE ${_chrono_cxx_flags})
endif()
if(CHRONO_LINKER_FLAGS)
    separate_arguments(_chrono_link_flags NATIVE_COMMAND "${CHRONO_LINKER_FLAGS}")
    target_link_options(wizengine PRIVATE ${_chrono_link_flags})
endif()

target_link_directories(wizengine PRIVATE ${FILAMENT_LIBDIR})

# Filament static libs, matched to a 1.74.x release's lib/x86_64/md contents.
# bluevk/bluegl are the Vulkan/GL loaders - bluevk provides the VkFormat symbols
# and must be linked on Windows too. 'shaders' replaced the old 'vkshaders'.
# If the linker reports a missing/absent .lib, adjust to your release's names.
set(FILAMENT_LIBS
    filament
    backend
    bluevk
    bluegl
    filabridge
    filaflat
    utils
    geometry
    smol-v
    shaders
    ibl
    basis_transcoder
    zstd)

# ---------------------------------------------------------------------------
# glTF / GLB loading (Filament's gltfio). The prebuilt releases ship it, but
# which helper libraries exist - and what they are called - varies between
# versions, so detect rather than assume. Anything in the optional list is
# linked when present and skipped when not; missing a required one turns the
# feature off and the build continues without model loading.
# ---------------------------------------------------------------------------
# Names as shipped in the 1.74 Windows release: the uberz library is called
# uberzlib there (uberz elsewhere), so accept either.
set(GLTFIO_REQUIRED gltfio_core uberarchive)
set(GLTFIO_UBERZ_NAMES uberzlib uberz)
# ktxreader and image are back here because gltfio's Ktx2Provider references
# them for KTX2-compressed model textures, even though the environment map no
# longer goes through a KTX file.
set(GLTFIO_OPTIONAL dracodec ktxreader image stb meshoptimizer mikktspace
                    basis_transcoder camutils)
# The environment map is converted at load time on the GPU, which needs
# filament-iblprefilter (and the generatePrefilterMipmap helper it calls).
list(APPEND FILAMENT_LIBS filament-iblprefilter filament-generatePrefilterMipmap)

function(wiz_find_filament_lib name out_var)
    unset(_wiz_lib CACHE)
    find_library(_wiz_lib NAMES ${name}
                 PATHS ${FILAMENT_ROOT}/lib/x86_64
                       ${FILAMENT_ROOT}/lib/x86_64/md
                       ${FILAMENT_ROOT}/lib/x86_64/mt
                 NO_DEFAULT_PATH)
    set(${out_var} ${_wiz_lib} PARENT_SCOPE)
endfunction()

set(WIZ_HAVE_GLTFIO TRUE)
set(GLTFIO_LIBS "")
foreach(_lib IN LISTS GLTFIO_REQUIRED)
    wiz_find_filament_lib(${_lib} _path)
    if(_path)
        list(APPEND GLTFIO_LIBS ${_lib})
    else()
        message(STATUS "WizEngine: required gltfio lib not found: ${_lib}")
        set(WIZ_HAVE_GLTFIO FALSE)
    endif()
endforeach()
# uberz under whichever name this release uses.
set(_uberz_found FALSE)
foreach(_lib IN LISTS GLTFIO_UBERZ_NAMES)
    wiz_find_filament_lib(${_lib} _path)
    if(_path AND NOT _uberz_found)
        list(APPEND GLTFIO_LIBS ${_lib})
        set(_uberz_found TRUE)
    endif()
endforeach()
if(NOT _uberz_found)
    message(STATUS "WizEngine: required gltfio lib not found: uberz/uberzlib")
    set(WIZ_HAVE_GLTFIO FALSE)
endif()

foreach(_lib IN LISTS GLTFIO_OPTIONAL)
    wiz_find_filament_lib(${_lib} _path)
    if(_path)
        list(APPEND GLTFIO_LIBS ${_lib})
    endif()
endforeach()

if(WIZ_HAVE_GLTFIO)
    message(STATUS "WizEngine: glTF/GLB loading enabled -> ${GLTFIO_LIBS}")
    list(APPEND FILAMENT_LIBS ${GLTFIO_LIBS})
    target_compile_definitions(wizengine PRIVATE WIZ_HAVE_GLTFIO)
else()
    message(WARNING
        "WizEngine: building without glTF/GLB support. Check which libraries "
        "exist under ${FILAMENT_ROOT}/lib/x86_64 and add them to "
        "GLTFIO_REQUIRED/GLTFIO_OPTIONAL in CMakeLists.txt.")
endif()

target_link_libraries(wizengine PRIVATE
    ${FILAMENT_LIBS}
    ${CHRONO_LIBRARIES}
    PkgConfig::GST)

# cpp-httplib needs a thread library on every platform (header-only).
find_package(Threads REQUIRED)
target_link_libraries(wizengine PRIVATE Threads::Threads)

if(UNIX AND NOT APPLE)
    target_link_libraries(wizengine PRIVATE ${CMAKE_DL_LIBS})
endif()

if(WIN32)
    # Filament's GL loader (bluegl) calls into Windows OpenGL/GDI; ws2_32 for httplib.
    # shlwapi: utils::Path (pulled in by gltfio) calls PathIsRelative.
    target_link_libraries(wizengine PRIVATE opengl32 gdi32 user32 ws2_32 shlwapi)
endif()
