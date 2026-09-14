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

# ---- B の任意モジュール（どれも OFF が既定 = 素の Chrono で configure が
# 通る）。ON にすると find_package にコンポーネントを足し、コードは
# WIZ_WITH_* と __has_include で該当ヘッダを取り込む。
#   PARSERS … URDF / OpenSim / ADAMS の取込（chrono_parsers。URDF は
#             Chrono 側で urdfdom が要る）
#   MODAL   … モーダル解析（chrono_modal。Spectra 同梱）
#   PARDISO … 直接法ソルバ PardisoMKL（Intel MKL が要る）
#   MUMPS   … 直接法ソルバ MUMPS
# FEA（ケーブル）と ChLoad（ブッシュ・定常荷重）は Chrono 本体に入っている
# のでオプションは無い。どれも Core バックエンドで動く（Scene が自動で切替）。
option(WIZ_WITH_CHRONO_PARSERS "Link Chrono::Parsers (URDF / OpenSim / ADAMS import)" OFF)
option(WIZ_WITH_CHRONO_MODAL "Link Chrono::Modal (modal analysis)" OFF)
option(WIZ_WITH_PARDISO "Link Chrono::PardisoMKL (direct solver)" OFF)
option(WIZ_WITH_MUMPS "Link Chrono::MUMPS (direct solver)" OFF)

set(_wiz_chrono_components "")
if(WIZ_USE_MULTICORE)
    list(APPEND _wiz_chrono_components Multicore)
endif()
if(WIZ_WITH_CHRONO_PARSERS)
    list(APPEND _wiz_chrono_components Parsers)
endif()
if(WIZ_WITH_CHRONO_MODAL)
    list(APPEND _wiz_chrono_components Modal)
endif()
if(WIZ_WITH_PARDISO)
    list(APPEND _wiz_chrono_components PardisoMKL)
endif()
if(WIZ_WITH_MUMPS)
    list(APPEND _wiz_chrono_components Mumps)
endif()

if(_wiz_chrono_components)
    find_package(Chrono COMPONENTS ${_wiz_chrono_components} CONFIG REQUIRED)
    message(STATUS "WizEngine: Chrono modules = ${_wiz_chrono_components}")
else()
    find_package(Chrono CONFIG REQUIRED)
endif()
if(WIZ_USE_MULTICORE)
    message(STATUS "WizEngine: physics engine = Chrono::Multicore (parallel, core fallback for FEA / loads / direct solvers)")
else()
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
# Compile the materials with matc -> *.filamat (next to the executable)
# ---------------------------------------------------------------------------
# Everything the program loads at runtime is gathered under one folder, so the
# executable has a single place to look and the whole set can be copied around
# together.
set(ASSET_DIR ${CMAKE_CURRENT_BINARY_DIR}/assets)
file(MAKE_DIRECTORY ${ASSET_DIR})
# エディタが保存するシーン（assets/scenes/*.xml）の置き場。実行時にも
# 作られるが、初回起動前から一覧に見えるように先に用意しておく。
file(MAKE_DIRECTORY ${ASSET_DIR}/scenes)

# assets/materials/*.mat をすべて matc に通す。追加は .mat を置くだけ
# （Renderer 側は assetPath("<name>.filamat") で読む）。
file(GLOB MAT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/assets/materials/*.mat)
set(MAT_OUTPUTS "")
foreach(_mat IN LISTS MAT_SOURCES)
    get_filename_component(_mat_name ${_mat} NAME_WE)
    set(_mat_out ${ASSET_DIR}/${_mat_name}.filamat)
    add_custom_command(
        OUTPUT ${_mat_out}
        COMMAND ${MATC} -a all -p desktop -o ${_mat_out} ${_mat}
        DEPENDS ${_mat}
        COMMENT "Compiling Filament material ${_mat_name}.mat (matc, all shader languages)")
    list(APPEND MAT_OUTPUTS ${_mat_out})
endforeach()

add_custom_target(materials ALL DEPENDS ${MAT_OUTPUTS})

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
# Header-only dependencies: cpp-httplib (control server), nlohmann/json
# (browser input), cgltf (glTF parsing for collision geometry) and stb_image
# (texture decoding).
#
# These live as git submodules under third_parties/ - fetch them with
#   git clone --recursive ...        (or)
#   git submodule update --init
# The submodules are pinned to the same versions the downloads used, and
# .gitmodules marks them shallow (nlohmann/json's full history is huge).
#
# A clone without --recursive must still build (same policy as FILAMENT_ROOT
# above), so any submodule that is missing falls back to downloading the
# single header into the build dir, exactly as before.
# ---------------------------------------------------------------------------
set(THIRD_PARTIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_parties")
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

# Use the submodule when its header is present; otherwise download the single
# header. Submodule include dirs are collected in WIZ_DEP_INCLUDE_DIRS and go
# in front of DEPS_INC, so a stale downloaded header can never shadow them.
set(WIZ_DEP_INCLUDE_DIRS "")

if(EXISTS "${THIRD_PARTIES_DIR}/cpp-httplib/httplib.h")
    list(APPEND WIZ_DEP_INCLUDE_DIRS "${THIRD_PARTIES_DIR}/cpp-httplib")
    message(STATUS "WizEngine: cpp-httplib -> submodule")
else()
    wiz_download_header(
        "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.15.3/httplib.h"
        "${DEPS_INC}/httplib.h")
endif()

if(EXISTS "${THIRD_PARTIES_DIR}/json/single_include/nlohmann/json.hpp")
    list(APPEND WIZ_DEP_INCLUDE_DIRS "${THIRD_PARTIES_DIR}/json/single_include")
    message(STATUS "WizEngine: nlohmann/json -> submodule")
else()
    wiz_download_header(
        "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp"
        "${DEPS_INC}/nlohmann/json.hpp")
endif()

# cgltf: parses glTF/GLB on the CPU for collision geometry (the renderer's
# gltfio streams vertices straight to the GPU, so physics reads the file
# itself - with the same parser gltfio uses internally).
if(EXISTS "${THIRD_PARTIES_DIR}/cgltf/cgltf.h")
    list(APPEND WIZ_DEP_INCLUDE_DIRS "${THIRD_PARTIES_DIR}/cgltf")
    message(STATUS "WizEngine: cgltf -> submodule")
else()
    wiz_download_header(
        "https://raw.githubusercontent.com/jkuhlmann/cgltf/v1.14/cgltf.h"
        "${DEPS_INC}/cgltf.h")
endif()

# stb_image: decodes PNG/JPEG/TGA/BMP for the ground texture.
if(EXISTS "${THIRD_PARTIES_DIR}/stb/stb_image.h")
    list(APPEND WIZ_DEP_INCLUDE_DIRS "${THIRD_PARTIES_DIR}/stb")
    message(STATUS "WizEngine: stb_image -> submodule")
else()
    wiz_download_header(
        "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"
        "${DEPS_INC}/stb_image.h")
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
# ソースは担当ごとのフォルダに分けてある（src/ が include のルートで、
# #include "core/Log.h" のようにフォルダ名から書く）:
#   core/       ログ・例外・版・計測・CPU ピン留めなど、エンジン非依存の土台
#   physics/    Chrono のラッパと glTF 凸包（当たり判定）
#   render/     Filament のラッパと画像 / HDR / glTF の読み込み
#   streaming/  HTTP・WebRTC・GStreamer の出力
#   document/   シーン文書の値型と MuJoCo 風 XML（エンジン非依存）
#   scene/      Scene の実体管理・エディタ状態・カメラ・数学の橋渡し
#   components/ ブラウザのコマンドを受ける SceneComponent 群
#   vehicle/    車両モデルとノード式（純粋な数値ライブラリ。tests/vehicle が使う）
set(WIZ_CORE_SOURCES
    src/core/AssetError.cpp
    src/core/CpuAffinity.cpp
    src/core/Log.cpp
    src/core/PortScan.cpp
    src/core/Versions.cpp)
set(WIZ_PHYSICS_SOURCES
    src/physics/MeshCollision.cpp
    src/physics/ModelImport.cpp
    src/physics/PhysicsWorld.cpp)
set(WIZ_RENDER_SOURCES
    src/render/EnvironmentLoader.cpp
    src/render/GltfLoader.cpp
    src/render/ImageLoader.cpp
    src/render/Renderer.cpp)
set(WIZ_STREAMING_SOURCES
    src/streaming/HttpServer.cpp
    src/streaming/VideoStreamer.cpp
    src/streaming/WebRtcStreamer.cpp)
set(WIZ_DOCUMENT_SOURCES
    src/document/SceneDocument.cpp
    src/document/SceneXml.cpp)
set(WIZ_SCENE_SOURCES
    src/scene/CameraObject.cpp
    src/scene/EditorState.cpp
    src/scene/PrefabDefaults.cpp
    src/scene/Scene.cpp
    src/scene/SceneEdit.cpp
    src/scene/SceneEvents.cpp
    src/scene/SceneSerialize.cpp
    src/scene/SoftLattice.cpp)
set(WIZ_COMPONENT_SOURCES
    src/components/EditorComponent.cpp
    src/components/GizmoComponent.cpp
    src/components/PrefabComponent.cpp
    src/components/VehicleComponent.cpp)
set(WIZ_VEHICLE_SOURCES
    src/vehicle/Formula.cpp
    src/vehicle/FormulaXml.cpp
    src/vehicle/LuaFormula.cpp
    src/vehicle/Powertrain.cpp
    src/vehicle/SoftTire.cpp
    src/vehicle/TireFormula.cpp
    src/vehicle/VehicleModel.cpp
    src/vehicle/VehicleTypes.cpp
    src/vehicle/VehicleXml.cpp
    src/vehicle/WheelController.cpp)

add_executable(wizengine
    src/main.cpp
    ${WIZ_CORE_SOURCES}
    ${WIZ_PHYSICS_SOURCES}
    ${WIZ_RENDER_SOURCES}
    ${WIZ_STREAMING_SOURCES}
    ${WIZ_DOCUMENT_SOURCES}
    ${WIZ_SCENE_SOURCES}
    ${WIZ_COMPONENT_SOURCES}
    ${WIZ_VEHICLE_SOURCES})
# ノード式の実行エンジン（LuaJIT）。無ければ C++ の参照インタプリタに落ちる。
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/LuaJIT.cmake)
wiz_link_luajit(wizengine)
# Version pins surfaced in the browser's About section (src/core/Versions.cpp).
# WIZ_CGLTF_VERSION must match the third_parties/cgltf submodule pin (and the
# wiz_download_header fallback URL above).
target_compile_definitions(wizengine PRIVATE
    WIZ_FILAMENT_VERSION="${FILAMENT_VERSION}"
    WIZ_CGLTF_VERSION="1.14")

add_dependencies(wizengine materials)

# Filament's public headers use designated initializers. clang/gcc accept these
# in C++17, but MSVC requires C++20 for them - so compile the translation units
# that include Filament headers as C++20. The rest of the project stays C++17
# (CMAKE_CXX_STANDARD above).
if(MSVC)
    set_source_files_properties(${WIZ_RENDER_SOURCES}
        PROPERTIES COMPILE_OPTIONS "/std:c++20")

    # Sources (and string literals) are UTF-8. Without this MSVC reads them as
    # the system code page - on a Japanese Windows that mangles the Japanese
    # text in log messages and comments (and once broke the HTML that used to
    # be embedded as a string literal).
    target_compile_options(wizengine PRIVATE /utf-8)
endif()

if(WIZ_USE_MULTICORE)
    target_compile_definitions(wizengine PRIVATE WIZ_USE_MULTICORE)
endif()
if(WIZ_WITH_CHRONO_PARSERS)
    target_compile_definitions(wizengine PRIVATE WIZ_WITH_CHRONO_PARSERS)
endif()
if(WIZ_WITH_CHRONO_MODAL)
    target_compile_definitions(wizengine PRIVATE WIZ_WITH_CHRONO_MODAL)
endif()
if(WIZ_WITH_PARDISO)
    target_compile_definitions(wizengine PRIVATE WIZ_WITH_PARDISO)
endif()
if(WIZ_WITH_MUMPS)
    target_compile_definitions(wizengine PRIVATE WIZ_WITH_MUMPS)
endif()

target_include_directories(wizengine PRIVATE
    ${FILAMENT_ROOT}/include
    ${CHRONO_INCLUDE_DIRS}
    ${WIZ_DEP_INCLUDE_DIRS}
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

# ---------------------------------------------------------------------------
# Tests. tests/vehicle は Chrono も Filament も要らない単体テストで、単独でも
# configure できる（tests/vehicle/CMakeLists.txt 参照）。本体と一緒に回すなら
#   cmake -B build -DWIZ_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
# ---------------------------------------------------------------------------
option(WIZ_BUILD_TESTS "Build the unit tests (vehicle, soft body, document) alongside the engine" OFF)
if(WIZ_BUILD_TESTS)
    enable_testing()
    # テストは単独でも configure できる（tests/<名前>/CMakeLists.txt）。
    # ここでは「あるものだけ」取り込む - 片方だけ持っているクローンでも
    # -DWIZ_BUILD_TESTS=ON が configure エラーにならないように。
    foreach(_test vehicle softbody document)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/${_test}/CMakeLists.txt)
            add_subdirectory(tests/${_test})
        endif()
    endforeach()
endif()
