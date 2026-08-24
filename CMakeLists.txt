# MERGEN — a minimal PDF viewer.
# Copyright (C) 2026 MEGAS.
# SPDX-License-Identifier: GPL-3.0-only

cmake_minimum_required(VERSION 3.25)

project(mergen
    VERSION 0.1.3
    DESCRIPTION "A minimal PDF viewer"
    HOMEPAGE_URL "https://github.com/sudo-megas/MERGEN"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(GNUInstallDirs)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets PrintSupport)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_QT6 REQUIRED IMPORTED_TARGET poppler-qt6)

# The privileged reader lives beside the binary, not on PATH, and is never
# installed setuid: polkit decides, not the filesystem.
set(MERGEN_HELPER_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/mergen/mergen-open"
    CACHE STRING "Installed path of the privileged PDF reader")

add_executable(mergen
    src/main.cpp
    src/mainwindow.cpp
    src/mainwindow.h
    src/pageview.cpp
    src/pageview.h
    src/document.cpp
    src/document.h)

target_compile_definitions(mergen PRIVATE
    MERGEN_VERSION="${PROJECT_VERSION}"
    MERGEN_HELPER_PATH="${MERGEN_HELPER_PATH}"
    QT_NO_CAST_FROM_ASCII
    QT_DISABLE_DEPRECATED_UP_TO=0x060500)

target_compile_options(mergen PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(mergen PRIVATE
    Qt6::Widgets
    Qt6::PrintSupport
    PkgConfig::POPPLER_QT6)

add_executable(mergen-open src/mergen-open.cpp)
target_compile_options(mergen-open PRIVATE -Wall -Wextra -Wpedantic)

configure_file(data/mergen.policy.in
    ${CMAKE_CURRENT_BINARY_DIR}/io.github.sudomegas.mergen.policy @ONLY)

install(TARGETS mergen RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS mergen-open RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/mergen)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.sudomegas.mergen.policy
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/polkit-1/actions)
