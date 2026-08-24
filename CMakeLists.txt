# MERGEN — a minimal PDF viewer.
# Copyright (C) 2026 MEGAS.
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.25)

project(mergen
    VERSION 2.0.5
    DESCRIPTION "A minimal PDF viewer"
    HOMEPAGE_URL "https://github.com/sudo-megas/MERGEN"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets PrintSupport Network)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)
pkg_check_modules(QPDF REQUIRED IMPORTED_TARGET libqpdf)

# The About dialog carries the licence in full; compile it in rather than
# depend on a file being present at runtime.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" MERGEN_LICENSE_TEXT)
configure_file(src/license.h.in ${CMAKE_CURRENT_BINARY_DIR}/license.h @ONLY)

set(MERGEN_RELEASE_DATE "2026-08-25" CACHE STRING "Date shown in the About dialog")

# The privileged reader lives beside the binary, not on PATH, and is never
# installed setuid: polkit decides, not the filesystem.
#
# This path is named in three places that must agree: the binary invokes it
# under pkexec, the generated policy authorises exactly it, and install() puts
# the helper there. It is deliberately NOT `set(... CACHE ...)` with a computed
# default — that form assigns only when the variable is absent from the cache,
# so reconfiguring an existing build tree with a different prefix keeps the old
# value while install() quietly moves the file. The binary and the policy then
# agree with each other about a path where no helper exists, pkexec fails on a
# missing file, and the reader is told the document "cannot be read": the dead
# end 2.0.2 was about, reached a different way. Nothing warns, and because no
# compile flag changed, ninja reports "no work to do" over the top of it.
#
# Recomputed every configure instead. An explicit -DMERGEN_HELPER_PATH still
# wins, because that lands in the cache before this runs.
if(NOT MERGEN_HELPER_PATH)
    set(MERGEN_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open")
endif()

# A tree configured before the above was written still holds the stale cached
# value, and an explicit override may simply be wrong. Say so either way: the
# helper is installed at the second path, and polkit will only authorise the
# first.
if(NOT MERGEN_HELPER_PATH STREQUAL "${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open")
    message(WARNING
        "MERGEN_HELPER_PATH is ${MERGEN_HELPER_PATH}, but install() puts the helper at "
        "${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open. The binary and the polkit policy "
        "will name the first; the file will be at the second, and elevation will fail. "
        "Configure a fresh build directory, or pass -DMERGEN_HELPER_PATH to match.")
endif()

add_executable(mergen
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/pageview.cpp
    src/pageview.h
    src/document.cpp
    src/document.h
    src/iconset.cpp
    src/iconset.h
    src/overlay.cpp
    src/overlay.h
    src/control.cpp
    src/control.h
    src/redact.cpp
    src/redact.h
    src/thumbstrip.cpp
    src/thumbstrip.h
    data/mergen.qrc)

target_compile_definitions(mergen PRIVATE
    MERGEN_VERSION="${PROJECT_VERSION}"
    MERGEN_HELPER_PATH="${MERGEN_HELPER_PATH}"
    MERGEN_RELEASE_DATE="${MERGEN_RELEASE_DATE}"
    QT_NO_CAST_FROM_ASCII
    QT_DISABLE_DEPRECATED_UP_TO=0x060500)

target_compile_options(mergen PRIVATE -Wall -Wextra -Wpedantic)

target_include_directories(mergen PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(mergen PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    Qt6::Network
    PkgConfig::POPPLER_QT6
    PkgConfig::QPDF)

add_executable(mergen-open src/mergen-open.cpp)
target_compile_options(mergen-open PRIVATE -Wall -Wextra -Wpedantic)

configure_file(data/mergen.policy.in
    ${CMAKE_CURRENT_BINARY_DIR}/io.github.sudomegas.mergen.policy @ONLY)

install(TARGETS mergen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS mergen-open RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/mergen)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.sudomegas.mergen.policy
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions)

install(FILES data/mergen.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)

install(FILES data/mergen.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps
    RENAME mergen.svg)

# Pre-rasterised so the build needs no SVG renderer, and so QIcon::fromTheme
# finds the icon without pulling in qt6-svg.
foreach(size 16 22 24 32 48 64 128 256 512)
    install(FILES data/icons/mergen-${size}.png
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/${size}x${size}/apps
        RENAME mergen.png)
endforeach()

install(FILES LICENSE
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/mergen)
