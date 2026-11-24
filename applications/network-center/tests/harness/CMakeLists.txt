# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test harnesses for the Network Center back-ends; not part of the
# product build (nothing references this directory). In the build chroot
# (SHADOW_SRC: the source tree, default /src):
#   cmake -S /src/applications/network-center/tests/harness -B /tmp/nc-harness/b -G Ninja && ninja -C /tmp/nc-harness/b
#   ncharness 'up|<profile>' 'ip|<profile>|<json>|1' ...   drives Nm (see main.cpp)
#   ncdiag 'ping|127.0.0.1' 'ts|' ...                      drives Diagnostics (see diag.cpp)
#   ncprivacy 'dns|dot|quad9|allow-downgrade' 'mac|per-network' ...
#                                                          drives NetPrivacy (see privacy.cpp;
#                                                          used by ../test_netprivacy_backend.py)
cmake_minimum_required(VERSION 3.25)
project(ncharness CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_AUTOMOC ON)
set(SHADOW_SRC "/src" CACHE PATH "Shadow OS source tree")
find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml DBus Network)
set(A ${SHADOW_SRC}/applications/network-center)
set(UI ${SHADOW_SRC}/desktop/ui/src/app)
add_executable(ncharness main.cpp ${A}/nmclient.cpp ${A}/nmclient.h ${A}/netutil.cpp ${A}/netutil.h ${UI}/command.cpp)
target_include_directories(ncharness PRIVATE ${A} ${UI})
target_compile_definitions(ncharness PRIVATE SHADOW_NETWORK_HELPER="/usr/libexec/shadow/network-helper")
target_link_libraries(ncharness PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus Qt6::Network)
add_executable(ncdiag diag.cpp ${A}/diagnostics.cpp ${A}/diagnostics.h ${A}/netstate.cpp ${A}/netstate.h ${A}/netutil.cpp ${A}/netutil.h ${UI}/command.cpp)
target_include_directories(ncdiag PRIVATE ${A} ${UI})
target_compile_definitions(ncdiag PRIVATE SHADOW_NETWORK_HELPER="/usr/libexec/shadow/network-helper")
target_link_libraries(ncdiag PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus Qt6::Network)
add_executable(ncprivacy privacy.cpp ${A}/netprivacy.cpp ${A}/netprivacy.h ${A}/netutil.cpp ${A}/netutil.h ${UI}/command.cpp)
target_include_directories(ncprivacy PRIVATE ${A} ${UI})
target_compile_definitions(ncprivacy PRIVATE SHADOW_NETWORK_HELPER="/usr/libexec/shadow/network-helper")
target_link_libraries(ncprivacy PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::DBus Qt6::Network)
