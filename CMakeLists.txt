cmake_minimum_required(VERSION 3.20)
project(SniPin LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_executable(SniPin WIN32
    src/main.cpp
    src/core.cpp
    src/resident.cpp
    src/worker.cpp
    src/selftest.cpp
    src/capture.cpp
    src/codec.cpp
    src/clipboard.cpp
    src/document.cpp
    src/pin.cpp
    src/settings_dialog.cpp
    src/resource.rc)

set(SNIPPIN_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/native/win-x64/static")
set(SNIPPIN_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/native/win-x64/include")

target_include_directories(SniPin PRIVATE src "${SNIPPIN_HEADERS}")
target_compile_definitions(SniPin PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(SniPin PRIVATE /W4 /permissive- /O1 /GL /Gy /Gw /GR- /EHsc- /utf-8 /GS)
target_link_options(SniPin PRIVATE /LTCG /OPT:REF /OPT:ICF /MANIFEST:NO)
target_link_libraries(SniPin PRIVATE
    user32 gdi32 shell32 advapi32 ole32 psapi gdiplus msimg32 comctl32 shcore
    "${SNIPPIN_NATIVE}/libwebp.lib"
    "${SNIPPIN_NATIVE}/libsharpyuv.lib")

set_target_properties(SniPin PROPERTIES OUTPUT_NAME "SniPin")
