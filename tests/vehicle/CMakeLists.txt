# 車両モデルの単体テスト。Chrono も Filament も要らない（src/vehicle/ と
# src/document/SceneXml.cpp だけを使う）ので、本体とは別に configure できる:
#
#   cmake -S tests/vehicle -B build-vehicle-test && cmake --build build-vehicle-test
#   ./build-vehicle-test/vehicle_test            （--csv out.csv で時系列を吐く）
#
# 本体の configure に -DWIZ_BUILD_TESTS=ON を付けると add_subdirectory で
# 取り込まれ、ctest から回せる。
cmake_minimum_required(VERSION 3.16)
project(wizengine_vehicle_test CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
add_executable(vehicle_test
    vehicle_test.cpp
    ${SRC}/vehicle/VehicleTypes.cpp
    ${SRC}/vehicle/WheelController.cpp
    ${SRC}/vehicle/Powertrain.cpp
    ${SRC}/vehicle/SoftTire.cpp
    ${SRC}/vehicle/VehicleModel.cpp
    ${SRC}/vehicle/VehicleXml.cpp
    ${SRC}/vehicle/Formula.cpp
    ${SRC}/vehicle/FormulaXml.cpp
    ${SRC}/vehicle/LuaFormula.cpp
    ${SRC}/vehicle/TireFormula.cpp
    ${SRC}/document/SceneXml.cpp)
target_include_directories(vehicle_test PRIVATE ${SRC})
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LuaJIT.cmake)
wiz_link_luajit(vehicle_test)
if(MSVC)
    target_compile_options(vehicle_test PRIVATE /W4 /utf-8)
else()
    target_compile_options(vehicle_test PRIVATE -Wall -Wextra)
endif()

# 単独 configure でも ctest が使えるように（本体からの add_subdirectory 時は
# 親の enable_testing() が効く）。
enable_testing()
add_test(NAME vehicle_test COMMAND vehicle_test)
