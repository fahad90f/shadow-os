# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test helpers of the shell's "status" module. Not part of the product build
# (nothing includes this directory); built on its own in the build chroot:
#
#   scripts/dev.sh exec -- cmake -S /src/desktop/horizon-shell/modules/status/tests \
#       -B /build/modstatus-tests -G Ninja
#   scripts/dev.sh exec -- ninja -C /build/modstatus-tests
#
# The binary lands in /build/modstatus-tests/bin, which the nested test
# session (scripts/dev.sh nested) puts on PATH.
cmake_minimum_required(VERSION 3.22)
project(shadow-status-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Qt6 REQUIRED COMPONENTS Core DBus)

add_executable(status-test-mpris-player mpris-test-player.cpp)
target_link_libraries(status-test-mpris-player PRIVATE Qt6::Core Qt6::DBus)

add_executable(status-test-dbus-fixture dbus-fixture.cpp)
target_link_libraries(status-test-dbus-fixture PRIVATE Qt6::Core Qt6::DBus)

# Self test of the kill switch / panic state source (parser and directory
# watcher); needs nothing but Qt Core. Run it directly:
#   /build/modstatus-tests/bin/status-test-killswitch
add_executable(status-test-killswitch killswitch-selftest.cpp
    ../statuskillswitch.cpp ../statuskillswitch.h ../statusbackend.cpp ../statusbackend.h)
set_target_properties(status-test-killswitch PROPERTIES AUTOMOC ON)
target_include_directories(status-test-killswitch PRIVATE ..)
target_compile_definitions(status-test-killswitch PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_link_libraries(status-test-killswitch PRIVATE Qt6::Core)
