# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test client of the system tray: a program with several status notifier
# items (KStatusNotifierItem). A project of its own, never part of the
# product build:
#
#   scripts/dev.sh exec --dir modtray -- cmake -S /src/desktop/horizon-shell/modules/tray/tests \
#       -B /build/modtray-tests -G Ninja
#   scripts/dev.sh exec --dir modtray -- ninja -C /build/modtray-tests
#
# and, in the nested session:  /build/modtray-tests/traytest [--hostile] [--status-interval MS]
cmake_minimum_required(VERSION 3.22)
project(tray-test-client LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets DBus)
find_package(KF6StatusNotifierItem REQUIRED)
add_executable(traytest traytest.cpp)
target_link_libraries(traytest PRIVATE Qt6::Widgets Qt6::DBus KF6::StatusNotifierItem)
