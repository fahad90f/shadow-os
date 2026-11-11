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

# NEXUS_DATA_ONLY configures just the architecture-independent data
# components (themes, artwork). It needs no compiler and no Qt, and is what
# the architecture-independent package build uses.
option(NEXUS_DATA_ONLY "Configure only the architecture-independent data components" OFF)
if(NEXUS_DATA_ONLY)
    set(_nexus_languages NONE)
else()
    set(_nexus_languages C CXX)
endif()

project(nexus-os VERSION ${NEXUS_VERSION} LANGUAGES ${_nexus_languages}
        DESCRIPTION "NEXUS OS desktop environment and system applications"
        HOMEPAGE_URL "https://github.com/fahad90fa/Custom-OS")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
if(NEXUS_DATA_ONLY)
    # No compiler means no library directory can be guessed; data components
    # install below share/ and etc/ only.
    set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "")
endif()
include(GNUInstallDirs)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Every component installs into its own CMake install component, named after
# the Debian package that ships it, so packaging is one
# "cmake --install --component <package>" per package (see debian/rules).
# Package of a component: "nexus-<directory name>", except for the entries here.
set(NEXUS_PACKAGE_OF_ui "nexus-ui")
set(NEXUS_PACKAGE_OF_gallery "nexus-ui-gallery")
set(NEXUS_PACKAGE_OF_horizon-shell "nexus-horizon")
set(NEXUS_PACKAGE_OF_installer "nexus-installer-gui")
set(NEXUS_PACKAGE_OF_manager "nexus-arsenal")
set(NEXUS_PACKAGE_OF_assets "nexus-artwork")

function(nexus_package_of out name)
    if(DEFINED NEXUS_PACKAGE_OF_${name})
        set(${out} "${NEXUS_PACKAGE_OF_${name}}" PARENT_SCOPE)
    else()
        set(${out} "nexus-${name}" PARENT_SCOPE)
    endif()
endfunction()

# ---- data components (no compiled code) -------------------------------------------
foreach(dir IN ITEMS assets desktop/themes)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        get_filename_component(name "${dir}" NAME)
        nexus_package_of(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME ${name})
        message(STATUS "NEXUS data component: ${name} (${dir}) -> ${CMAKE_INSTALL_DEFAULT_COMPONENT_NAME}")
        add_subdirectory(${dir})
    endif()
endforeach()
if(NEXUS_DATA_ONLY)
    return()
endif()

# ---- compiled components -------------------------------------------------------------
include(NexusDefaults)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Widgets DBus Svg Concurrent Network)
qt_standard_project_setup(REQUIRES 6.8)

# Components are discovered, not listed: every directory matched below that
# contains a CMakeLists.txt is a component named after the directory. Adding
# an application therefore never requires editing this file.
set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "nexus-ui")
add_subdirectory(desktop/ui)          # shared foundation, always built

set(NEXUS_ONLY "" CACHE STRING "Semicolon-separated component names to build (default: all)")

file(GLOB _component_lists CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
    desktop/ui/gallery/CMakeLists.txt
    desktop/*/CMakeLists.txt
    applications/*/CMakeLists.txt
    system/installer/CMakeLists.txt
    system/recovery/CMakeLists.txt
    arsenal/manager/CMakeLists.txt)
list(REMOVE_ITEM _component_lists desktop/ui/CMakeLists.txt desktop/themes/CMakeLists.txt)
list(SORT _component_lists)

set(_known_components "")
foreach(listfile IN LISTS _component_lists)
    get_filename_component(dir "${listfile}" DIRECTORY)
    get_filename_component(name "${dir}" NAME)
    list(APPEND _known_components "${name}")
    if(NEXUS_ONLY AND NOT name IN_LIST NEXUS_ONLY)
        continue()
    endif()
    nexus_package_of(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME ${name})
    message(STATUS "NEXUS component: ${name} (${dir}) -> ${CMAKE_INSTALL_DEFAULT_COMPONENT_NAME}")
    add_subdirectory(${dir})
endforeach()

foreach(wanted IN LISTS NEXUS_ONLY)
    if(NOT wanted IN_LIST _known_components)
        message(FATAL_ERROR "NEXUS_ONLY names unknown component '${wanted}' (known: ${_known_components})")
    endif()
endforeach()
