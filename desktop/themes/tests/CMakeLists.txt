# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test helpers of desktop/themes. Not part of the product build (the
# top-level CMakeLists.txt does not descend into this directory); built on
# demand by the test scripts next to this file:
#
#   cmake -S desktop/themes/tests -B <dir> -G Ninja && cmake --build <dir>
cmake_minimum_required(VERSION 3.25)
project(shadow-themes-tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_compile_options(-Wall -Wextra)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network DBus)

# A Qt Widgets application that knows nothing about SHADOW (the look of
# third-party Qt/KDE applications).
qt_add_executable(shadow-themes-widgets-demo widgets-demo/main.cpp)
target_link_libraries(shadow-themes-widgets-demo PRIVATE Qt6::Widgets)
