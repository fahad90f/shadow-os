# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test harnesses for the Privacy page's back-ends; not part of the
# product build (nothing references this directory). In the build chroot, with
# the source tree at SHADOW_SRC (default /src):
#   cmake -S <tree>/applications/control-center/tests/harness -B /tmp/cc-harness -G Ninja -DSHADOW_SRC=<tree>
#   ninja -C /tmp/cc-harness
#   cckillswitch 'set:tor' 'lan:1' 'keep:0' 'reassert' 'release' 'check' 'show' ...
#   cchideme 'ghost-on' 'start:30' 'ks:1' 'harden:fs' 'show' ...
# Used by ../test_killswitch_backend.py and ../test_hideme_backend.py.
cmake_minimum_required(VERSION 3.25)
project(ccharness CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(SHADOW_SRC "/src" CACHE PATH "Shadow OS source tree")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml DBus)
set(A ${SHADOW_SRC}/applications/control-center)
add_executable(cckillswitch main.cpp
    ${A}/backends/privacykillswitchbackend.cpp ${A}/backends/privacykillswitchbackend.h
    ${A}/dumpregistry.cpp ${A}/dumpregistry.h
    ${SHADOW_SRC}/desktop/ui/src/app/command.cpp)
target_include_directories(cckillswitch PRIVATE ${A} ${A}/backends ${SHADOW_SRC}/desktop/ui/src/app)
target_link_libraries(cckillswitch PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus)

add_executable(cchideme hideme.cpp
    ${A}/backends/privacybackend.cpp ${A}/backends/privacybackend.h
    ${A}/dumpregistry.cpp ${A}/dumpregistry.h)
target_include_directories(cchideme PRIVATE ${A} ${A}/backends)
target_link_libraries(cchideme PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus)
