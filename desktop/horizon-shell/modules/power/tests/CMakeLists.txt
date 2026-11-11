# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test helper of the power module; a project of its own, never part of the
# product build:
#
#   scripts/dev.sh exec --dir modpower -- cmake -S /src/desktop/horizon-shell/modules/power/tests \
#       -B /build/modpower-tests -G Ninja
#   scripts/dev.sh exec --dir modpower -- ninja -C /build/modpower-tests
#
# power-test-idle-inhibit: a window that holds a Wayland idle inhibitor
# (zwp_idle_inhibit_manager_v1) while it is shown. Used by session-test.sh
# when it is on PATH (/build/modpower-tests/bin).
cmake_minimum_required(VERSION 3.22)
project(power-module-tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
find_package(Qt6 REQUIRED COMPONENTS Gui WaylandClient)
find_package(PkgConfig REQUIRED)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
add_executable(power-test-idle-inhibit idle-inhibit-window.cpp)
qt6_generate_wayland_protocol_client_sources(power-test-idle-inhibit
    FILES ${WAYLAND_PROTOCOLS_DIR}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml)
target_link_libraries(power-test-idle-inhibit PRIVATE Qt6::Gui Qt6::GuiPrivate Qt6::WaylandClient)
