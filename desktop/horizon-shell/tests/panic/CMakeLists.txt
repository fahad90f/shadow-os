# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone test of the shell's panic action (src/panic.h), not part of the
# package build: run.sh (or inner.sh) builds and runs it.
cmake_minimum_required(VERSION 3.25)
project(panictest C CXX)
set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The Panic sources under test (default: the shell's own).
set(PANIC_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../src CACHE PATH "directory with panic.cpp and panic.h")
find_package(Qt6 REQUIRED COMPONENTS Core DBus Gui Qml)
add_executable(panic-test test.cpp ${PANIC_SRC_DIR}/panic.cpp ${PANIC_SRC_DIR}/panic.h)
target_include_directories(panic-test PRIVATE ${PANIC_SRC_DIR})
# As the shell is built (cmake/ShadowDefaults.cmake).
target_compile_definitions(panic-test PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII QT_NO_KEYWORDS)
target_link_libraries(panic-test PRIVATE Qt6::Core Qt6::DBus Qt6::Gui Qt6::Qml)
# pkexec after the administrator authenticated (installed setuid root by the test).
add_executable(root-pkexec root-pkexec.c)
