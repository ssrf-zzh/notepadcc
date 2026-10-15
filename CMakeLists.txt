cmake_minimum_required(VERSION 3.25)

project(notepadcc
    VERSION 1.0.0
    DESCRIPTION "notepadcc - lightweight native Windows text/source editor"
    LANGUAGES CXX)

# 资源编译器：MinGW 用 windres，MSVC 用 rc.exe。缺失时跳过图标与清单，不影响编译。
enable_language(RC OPTIONAL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(NPCC_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH "vendored dependencies")
set(NPCC_SCINTILLA  "${NPCC_THIRD_PARTY}/scintilla")
set(NPCC_LEXILLA    "${NPCC_THIRD_PARTY}/lexilla")

if (NOT EXISTS "${NPCC_SCINTILLA}/include/Scintilla.h" OR NOT EXISTS "${NPCC_LEXILLA}/include/Lexilla.h")
    message(FATAL_ERROR
        "找不到第三方源码。请先运行：\n"
        "    powershell -ExecutionPolicy Bypass -File scripts/fetch_deps.ps1\n"
        "或手动把 Scintilla / Lexilla 解压到 third_party/ 下（目录名必须是 scintilla 与 lexilla）。")
endif()

# ---------------------------------------------------------------- 公共编译选项
add_library(npcc_flags INTERFACE)
target_compile_definitions(npcc_flags INTERFACE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00)          # Windows 10

if (MSVC)
    target_compile_options(npcc_flags INTERFACE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP)
    target_compile_definitions(npcc_flags INTERFACE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(npcc_flags INTERFACE
        -Wall -Wextra -Wpedantic -Wno-unknown-pragmas -Wno-unused-parameter)
endif()

# 第三方代码不开自家警告（它们有自己的标准，且我们不改它们）
set(NPCC_VENDOR_FLAGS "")
if (MSVC)
    set(NPCC_VENDOR_FLAGS /W0)
else()
    set(NPCC_VENDOR_FLAGS -w)
endif()

# ---------------------------------------------------------------- Scintilla 静态库
# 官方 win32/makefile 的 COMPONENT_OBJS = scintilla/src/*.cxx + scintilla/win32/*.cxx（不含 ScintillaDLL.cxx，
# 那是构建 DLL 用的导出层，静态链接时不需要）。
file(GLOB NPCC_SCINTILLA_SRC CONFIGURE_DEPENDS "${NPCC_SCINTILLA}/src/*.cxx")
file(GLOB NPCC_SCINTILLA_WIN CONFIGURE_DEPENDS "${NPCC_SCINTILLA}/win32/*.cxx")
list(FILTER NPCC_SCINTILLA_WIN EXCLUDE REGEX "ScintillaDLL\\.cxx$")

add_library(scintilla STATIC ${NPCC_SCINTILLA_SRC} ${NPCC_SCINTILLA_WIN})
target_include_directories(scintilla PUBLIC
    "${NPCC_SCINTILLA}/include"
    "${NPCC_SCINTILLA}/src")
target_compile_options(scintilla PRIVATE ${NPCC_VENDOR_FLAGS})
target_compile_definitions(scintilla PRIVATE NDEBUG)
target_link_libraries(scintilla PUBLIC
    gdi32 user32 imm32 ole32 uuid oleaut32 advapi32)

# ---------------------------------------------------------------- Lexilla 静态库
file(GLOB NPCC_LEXILLA_SRC   CONFIGURE_DEPENDS "${NPCC_LEXILLA}/src/*.cxx")
file(GLOB NPCC_LEXILLA_LEXLIB CONFIGURE_DEPENDS "${NPCC_LEXILLA}/lexlib/*.cxx")
file(GLOB NPCC_LEXILLA_LEXERS CONFIGURE_DEPENDS "${NPCC_LEXILLA}/lexers/*.cxx")

add_library(lexilla STATIC
    ${NPCC_LEXILLA_SRC} ${NPCC_LEXILLA_LEXLIB} ${NPCC_LEXILLA_LEXERS})
target_include_directories(lexilla PUBLIC
    "${NPCC_LEXILLA}/include"
    "${NPCC_LEXILLA}/lexlib"
    "${NPCC_SCINTILLA}/include")
target_compile_options(lexilla PRIVATE ${NPCC_VENDOR_FLAGS})
target_compile_definitions(lexilla PRIVATE NDEBUG)

# ---------------------------------------------------------------- 核心静态库（主程序与测试共用）
set(NPCC_CORE_SOURCES
    src/core/CommandRegistry.cpp
    src/edit/EditView.cpp
    src/edit/Encoding.cpp
    src/infra/Log.cpp
    src/infra/Paths.cpp
    src/io/AtomicFile.cpp
    src/io/FileReader.cpp
    src/shell/MainWindow.cpp
    src/shell/MenuBuilder.cpp
    src/shell/StatusBar.cpp)

add_library(notepadcc_core STATIC ${NPCC_CORE_SOURCES})
target_include_directories(notepadcc_core PUBLIC src)
target_link_libraries(notepadcc_core PUBLIC
    scintilla lexilla npcc_flags comctl32 comdlg32 shell32)

# 链接与代码生成选项：主程序与测试共用
add_library(npcc_app_options INTERFACE)
if (MINGW)
    target_link_options(npcc_app_options INTERFACE
        -static -static-libgcc -static-libstdc++ -Wl,--gc-sections)
    target_compile_options(npcc_app_options INTERFACE -ffunction-sections -fdata-sections)
endif()
if (MSVC)
    target_link_options(npcc_app_options INTERFACE /OPT:REF /OPT:ICF)
endif()

# ---------------------------------------------------------------- notepadcc 主程序
add_executable(notepadcc WIN32 src/app/main.cpp)
target_link_libraries(notepadcc PRIVATE notepadcc_core npcc_app_options)

if (CMAKE_RC_COMPILER_LOADED)
    target_sources(notepadcc PRIVATE res/notepadcc.rc)
else()
    message(WARNING "未找到资源编译器（windres / rc.exe），本次构建将不含图标与清单")
endif()

if (MINGW)
    # -municode 提供 wWinMain 入口
    target_link_options(notepadcc PRIVATE -municode -mwindows)
endif()

# 输出目录统一，便于打包与运行
set_target_properties(notepadcc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    OUTPUT_NAME "notepadcc")

# ---------------------------------------------------------------- 单元测试
option(NPCC_BUILD_TESTS "构建单元测试" ON)
if (NPCC_BUILD_TESTS)
    add_executable(notepadcc_tests tests/unit/test_main.cpp)
    target_link_libraries(notepadcc_tests PRIVATE notepadcc_core npcc_app_options)
    set_target_properties(notepadcc_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

message(STATUS "notepadcc 配置完成")
message(STATUS "  Scintilla : ${NPCC_SCINTILLA}")
message(STATUS "  Lexilla   : ${NPCC_LEXILLA}")
message(STATUS "  编译器    : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
