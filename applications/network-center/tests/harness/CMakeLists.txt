# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test harnesses for the Network Center back-ends; not part of the
# product build (nothing references this directory). In the build chroot:
#   cmake -S /src/applications/network-center/tests/harness -B /tmp/nc-harness/b -G Ninja && ninja -C /tmp/nc-harness/b
#   ncharness 'up|<profile>' 'ip|<profile>|<json>|1' ...   drives Nm (see main.cpp)
#   ncdiag 'ping|127.0.0.1' 'ts|' ...                      drives Diagnostics (see diag.cpp)
cmake_minimum_required(VERSION 3.25)
project(ncharness CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml DBus Network)
set(A /src/applications/network-center)
add_executable(ncharness main.cpp ${A}/nmclient.cpp ${A}/nmclient.h ${A}/netutil.cpp ${A}/netutil.h /src/desktop/ui/src/app/command.cpp)
target_include_directories(ncharness PRIVATE ${A} /src/desktop/ui/src/app)
target_compile_definitions(ncharness PRIVATE SHADOW_NETWORK_HELPER="/usr/libexec/shadow/network-helper")
target_link_libraries(ncharness PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus Qt6::Network)
add_executable(ncdiag diag.cpp ${A}/diagnostics.cpp ${A}/diagnostics.h ${A}/netstate.cpp ${A}/netstate.h ${A}/netutil.cpp ${A}/netutil.h /src/desktop/ui/src/app/command.cpp)
target_include_directories(ncdiag PRIVATE ${A} /src/desktop/ui/src/app)
target_compile_definitions(ncdiag PRIVATE SHADOW_NETWORK_HELPER="/usr/libexec/shadow/network-helper")
target_link_libraries(ncdiag PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus Qt6::Network)
