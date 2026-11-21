# Build Loupe on Windows with Visual Studio 2022 (Desktop C++ workload):
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# Or cross-compile from Linux/macOS with ./build.sh (Zig).
cmake_minimum_required(VERSION 3.20)
project(Loupe LANGUAGES C CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT, no redistributable needed

file(GLOB WEBP_SOURCES CONFIGURE_DEPENDS
  third_party/libwebp/src/dec/*.c third_party/libwebp/src/demux/*.c
  third_party/libwebp/src/utils/*.c third_party/libwebp/src/dsp/*.c)
add_library(webpdec STATIC ${WEBP_SOURCES})
target_include_directories(webpdec PUBLIC third_party/libwebp)

file(GLOB LOUPE_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_executable(Loupe WIN32 ${LOUPE_SOURCES} res/loupe.rc)
target_compile_definitions(Loupe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
  target_compile_options(Loupe PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(Loupe PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)   # manifest comes from loupe.rc
endif()
target_link_libraries(Loupe PRIVATE webpdec d2d1 dwrite d3d11 dxgi ole32 oleaut32 uuid dwmapi shcore shell32 user32 gdi32 advapi32
  shlwapi windowscodecs comctl32 uxtheme winhttp bcrypt version comdlg32 propsys)
set_target_properties(Loupe PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
