# 清风输入法 Fcitx4 addon（X11）。
#
# 与 wind_linux（Fcitx5 addon）共享纯逻辑核（src/core）与 X11 候选窗实现：
#   - src/core 不依赖任何 fcitx 头文件，原样编译；
#   - 候选窗 X11Panel 只把 fcitx5 的 event/log 头换成 CompatEvent/CompatLog（副本，逻辑未改）；
#   - 宿主适配层是 src/WindEngine4.cpp（fcitx4 IM API）。
#
# 依赖：fcitx4 开发包（pkg-config fcitx）、libxcb + xcb-shape + xcb-randr。
# 构建：
#   cmake -S wind_linux_fcitx4 -B wind_linux_fcitx4/build -DCMAKE_BUILD_TYPE=Release
#   cmake --build wind_linux_fcitx4/build -j4
cmake_minimum_required(VERSION 3.16)
project(windinput_fcitx4 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(WIND_VARIANT_DEV "Build the dev variant (WindInputDev runtime dir)" OFF)
set(WIND_SETTING_PATH "" CACHE STRING "设置程序默认路径（空 = 源码内置 /usr/lib/windinput/wind_setting）")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FCITX4 REQUIRED IMPORTED_TARGET fcitx)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb xcb-shape xcb-randr)
find_package(Threads REQUIRED)

set(CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../wind_linux")
set(CORE_SOURCES
    ${CORE_DIR}/src/core/Bridge.cpp
    ${CORE_DIR}/src/core/Codec.cpp
    ${CORE_DIR}/src/core/GnomeCaretGeom.cpp
    ${CORE_DIR}/src/core/HostUi.cpp
    ${CORE_DIR}/src/core/KeyMap.cpp
    ${CORE_DIR}/src/core/KeySynth.cpp
    ${CORE_DIR}/src/core/Menu.cpp
    ${CORE_DIR}/src/core/OverlayInput.cpp
    ${CORE_DIR}/src/core/ResponseRouter.cpp
    ${CORE_DIR}/src/core/RetiredSurfaces.cpp
    ${CORE_DIR}/src/core/ServiceLauncher.cpp
    ${CORE_DIR}/src/core/SettingsLauncher.cpp
    ${CORE_DIR}/src/core/ShmFrame.cpp
    ${CORE_DIR}/src/core/Utf.cpp
)

add_library(windcore STATIC ${CORE_SOURCES})
target_include_directories(windcore PUBLIC ${CORE_DIR}/include)
target_compile_options(windcore PRIVATE -Wall -Wextra)
target_compile_definitions(windcore PRIVATE WIND_VARIANT_DEV=$<BOOL:${WIND_VARIANT_DEV}>)
if(WIND_SETTING_PATH)
    target_compile_definitions(windcore PUBLIC WIND_SETTING_PATH="${WIND_SETTING_PATH}")
endif()
target_link_libraries(windcore PUBLIC Threads::Threads)

add_library(fcitx-windinput MODULE
    src/CompatEvent.cpp
    src/X11Panel.cpp
    src/WindEngine4.cpp
    src/Addon.cpp
)
set_target_properties(fcitx-windinput PROPERTIES PREFIX "" OUTPUT_NAME "fcitx-windinput")
target_include_directories(fcitx-windinput PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CORE_DIR}/src/fcitx
    ${CORE_DIR}/include
)
target_compile_options(fcitx-windinput PRIVATE -Wall -Wextra)
target_link_libraries(fcitx-windinput PRIVATE windcore PkgConfig::FCITX4 PkgConfig::XCB)
# GCC 8（Debian buster）的 std::filesystem 在独立的 libstdc++fs 里；GCC 9+ 已并入 libstdc++。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    target_link_libraries(fcitx-windinput PRIVATE stdc++fs)
endif()

include(GNUInstallDirs)
# fcitx4 只从编译期 libdir 下的 fcitx/ 加载 addon（Debian 系是 /usr/lib/<multiarch>/fcitx，
# 不是 /usr/lib/fcitx）。直接用 pkg-config 报告的 libdir，避免装错位置。
pkg_get_variable(FCITX4_LIBDIR fcitx libdir)
if(NOT FCITX4_LIBDIR)
    set(FCITX4_LIBDIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS fcitx-windinput LIBRARY DESTINATION ${FCITX4_LIBDIR}/fcitx)
install(FILES data/fcitx-windinput.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/fcitx/addon)

# 图标：沿用 wind_linux 的成品（应用图标 + 托盘种子图标）。
file(GLOB_RECURSE WIND_ICONS RELATIVE "${CORE_DIR}/data/icons"
     "${CORE_DIR}/data/icons/hicolor/*/*.png")
foreach(icon ${WIND_ICONS})
    get_filename_component(icon_dir "${icon}" DIRECTORY)
    install(FILES "${CORE_DIR}/data/icons/${icon}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/${icon_dir}")
endforeach()
