# SPDX-License-Identifier: GPL-3.0-or-later
#
# Self test of the notification text sanitiser. A project of its own, never
# part of the product build:
#
#   scripts/dev.sh exec --dir modnotif -- cmake -S /src/desktop/horizon-shell/modules/notifications/tests \
#       -B /build/modnotif-tests -G Ninja
#   scripts/dev.sh exec --dir modnotif -- ninja -C /build/modnotif-tests
#   scripts/dev.sh exec --dir modnotif -- /build/modnotif-tests/notificationselftest
cmake_minimum_required(VERSION 3.22)
project(notification-selftest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Core)
add_executable(notificationselftest notificationselftest.cpp ../notificationtext.cpp ../notificationtext.h)
target_include_directories(notificationselftest PRIVATE ..)
target_compile_definitions(notificationselftest PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_link_libraries(notificationselftest PRIVATE Qt6::Core)
