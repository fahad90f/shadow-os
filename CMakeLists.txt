# NEXUS OS — top-level build of every compiled component (desktop shell, UI
# library, system applications, installer front-end).
#
#   cmake -S . -B build/cmake -G Ninja && cmake --build build/cmake
#
# Normally driven by debian/rules inside the Debian build chroot
# (build/packages/build.sh). To work on one component only:
#
#   cmake -S . -B <dir> -G Ninja -DNEXUS_ONLY="files;monitor"
#
# which configures the shared UI library plus just the named components.

cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/distro/base/nexus.conf" _nexus_version_line REGEX "^NEXUS_VERSION=")
string(REGEX REPLACE "^NEXUS_VERSION=\"?([0-9.]+)\"?.*$" "\\1" NEXUS_VERSION "${_nexus_version_line}")

project(nexus-os VERSION ${NEXUS_VERSION} LANGUAGES C CXX
        DESCRIPTION "NEXUS OS desktop environment and system applications"
        HOMEPAGE_URL "https://github.com/fahad90fa/Custom-OS")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(NexusDefaults)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets DBus Svg Concurrent Network)
qt_standard_project_setup(REQUIRES 6.8)

# Components are discovered, not listed: every directory matched below that
# contains a CMakeLists.txt is a component named after the directory. Adding
# an application therefore never requires editing this file.
add_subdirectory(desktop/ui)          # shared foundation, always built

set(NEXUS_ONLY "" CACHE STRING "Semicolon-separated component names to build (default: all)")

file(GLOB _component_lists CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    desktop/ui/gallery/CMakeLists.txt
    desktop/*/CMakeLists.txt
    applications/*/CMakeLists.txt
    system/installer/CMakeLists.txt
    system/recovery/CMakeLists.txt
    arsenal/manager/CMakeLists.txt)
list(REMOVE_ITEM _component_lists desktop/ui/CMakeLists.txt)
list(SORT _component_lists)

set(_known_components "")
foreach(listfile IN LISTS _component_lists)
    get_filename_component(dir "${listfile}" DIRECTORY)
    get_filename_component(name "${dir}" NAME)
    list(APPEND _known_components "${name}")
    if(NEXUS_ONLY AND NOT name IN_LIST NEXUS_ONLY)
        continue()
    endif()
    message(STATUS "NEXUS component: ${name} (${dir})")
    add_subdirectory(${dir})
endforeach()

foreach(wanted IN LISTS NEXUS_ONLY)
    if(NOT wanted IN_LIST _known_components)
        message(FATAL_ERROR "NEXUS_ONLY names unknown component '${wanted}' (known: ${_known_components})")
    endif()
endforeach()
