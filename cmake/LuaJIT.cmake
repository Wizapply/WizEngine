# LuaJIT（ノード式の実行エンジン）の検出。本体の CMakeLists.txt と
# tests/vehicle/CMakeLists.txt の両方から include する。
#
#   WIZ_WITH_LUAJIT   … OFF にすると LuaJIT 無しでビルド（ノード式は C++ の
#                       参照インタプリタで動く。遅いが結果は同じ）
#   WIZ_LUAJIT_ROOT   … LuaJIT のソース（既定 third_parties/luajit）または
#                       インストール先。src/lua.h と静的ライブラリを探す
#   WIZ_LUAJIT_BUILD  … ライブラリが無ければ configure 時に作る（既定 ON）。
#                       Linux/macOS は make BUILDMODE=static、Windows は
#                       msvcbuild.bat static（VS の開発者環境 = cl が PATH に
#                       ある configure で動く。VS の「フォルダーを開く」はこれ）
#
# 結果: WIZ_LUAJIT_FOUND / WIZ_LUAJIT_INCLUDE_DIR / WIZ_LUAJIT_LIBRARY と
# 関数 wiz_link_luajit(<target>)（インクルード・リンク・WIZ_HAVE_LUAJIT の定義）。
option(WIZ_WITH_LUAJIT "Run node formulas with LuaJIT (off = C++ interpreter)" ON)
option(WIZ_LUAJIT_BUILD "Build LuaJIT from WIZ_LUAJIT_ROOT if the library is missing" ON)
get_filename_component(_wiz_luajit_default "${CMAKE_CURRENT_LIST_DIR}/../third_parties/luajit" ABSOLUTE)
set(WIZ_LUAJIT_ROOT "${_wiz_luajit_default}" CACHE PATH "LuaJIT source or install root")

set(WIZ_LUAJIT_FOUND OFF)
set(WIZ_LUAJIT_INCLUDE_DIR "")
set(WIZ_LUAJIT_LIBRARY "")

function(_wiz_luajit_locate)
    set(inc "")
    foreach(cand "${WIZ_LUAJIT_ROOT}/src" "${WIZ_LUAJIT_ROOT}/include/luajit-2.1"
                 "${WIZ_LUAJIT_ROOT}/include")
        if(EXISTS "${cand}/luajit.h")
            set(inc "${cand}")
            break()
        endif()
    endforeach()
    set(lib "")
    foreach(cand "${WIZ_LUAJIT_ROOT}/src/libluajit.a" "${WIZ_LUAJIT_ROOT}/src/lua51.lib"
                 "${WIZ_LUAJIT_ROOT}/src/libluajit-5.1.a" "${WIZ_LUAJIT_ROOT}/lib/libluajit-5.1.a"
                 "${WIZ_LUAJIT_ROOT}/lib/lua51.lib" "${WIZ_LUAJIT_ROOT}/lib/libluajit.a")
        if(EXISTS "${cand}")
            set(lib "${cand}")
            break()
        endif()
    endforeach()
    set(WIZ_LUAJIT_INCLUDE_DIR "${inc}" PARENT_SCOPE)
    set(WIZ_LUAJIT_LIBRARY "${lib}" PARENT_SCOPE)
endfunction()

if(WIZ_WITH_LUAJIT)
    _wiz_luajit_locate()
    if(WIZ_LUAJIT_INCLUDE_DIR AND NOT WIZ_LUAJIT_LIBRARY AND WIZ_LUAJIT_BUILD
       AND EXISTS "${WIZ_LUAJIT_ROOT}/src/Makefile")
        message(STATUS "WizEngine: building LuaJIT in ${WIZ_LUAJIT_ROOT}/src ...")
        if(WIN32)
            execute_process(
                COMMAND cmd /c msvcbuild.bat static
                WORKING_DIRECTORY "${WIZ_LUAJIT_ROOT}/src"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        else()
            execute_process(
                COMMAND make -j4 BUILDMODE=static
                WORKING_DIRECTORY "${WIZ_LUAJIT_ROOT}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
        endif()
        if(NOT _rc EQUAL 0)
            message(WARNING "WizEngine: LuaJIT build failed (${_rc}); node formulas will use "
                            "the C++ interpreter.\n${_err}")
        endif()
        _wiz_luajit_locate()
    endif()
    if(WIZ_LUAJIT_INCLUDE_DIR AND WIZ_LUAJIT_LIBRARY)
        set(WIZ_LUAJIT_FOUND ON)
        message(STATUS "WizEngine: LuaJIT -> ${WIZ_LUAJIT_LIBRARY}")
    else()
        message(STATUS "WizEngine: LuaJIT not found (WIZ_LUAJIT_ROOT=${WIZ_LUAJIT_ROOT}); "
                       "node formulas will use the C++ interpreter. "
                       "git submodule update --init third_parties/luajit")
    endif()
endif()

function(wiz_link_luajit target)
    if(WIZ_LUAJIT_FOUND)
        target_include_directories(${target} PRIVATE "${WIZ_LUAJIT_INCLUDE_DIR}")
        target_link_libraries(${target} PRIVATE "${WIZ_LUAJIT_LIBRARY}")
        target_compile_definitions(${target} PRIVATE WIZ_HAVE_LUAJIT)
        if(UNIX AND NOT APPLE)
            target_link_libraries(${target} PRIVATE dl m)
        endif()
    endif()
endfunction()
