# Shared compiler settings and helper functions for all SHADOW components.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All executables in <build>/bin and shared libraries in <build>/lib, so the
# development scripts and tests can find them without knowing the source layout.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
                    -Wformat=2 -Werror=format-security -Werror=return-type)
add_compile_definitions(
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_URL_CAST_FROM_STRING
    QT_NO_KEYWORDS
    QT_STRICT_ITERATORS
    QT_DISABLE_DEPRECATED_BEFORE=0x060800
    SHADOW_VERSION="${PROJECT_VERSION}"
)

# Where QML modules are installed (matches Debian's Qt 6 layout).
set(SHADOW_QML_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/qt6/qml" CACHE PATH "QML module install directory")
# QML modules are built into <build>/qml so uninstalled binaries find them.
set(SHADOW_QML_BUILD_DIR "${CMAKE_BINARY_DIR}/qml")
set(QT_QML_OUTPUT_DIRECTORY "${SHADOW_QML_BUILD_DIR}")
set(SHADOW_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/shadow" CACHE PATH "Private helper executables")
set(SHADOW_DATA_DIR "${CMAKE_INSTALL_DATADIR}/shadow" CACHE PATH "Shared data")

# shadow_add_app(<target> SOURCES ... QML ... [RESOURCES ...] [LIBS ...]
#               URI <qml uri> DESKTOP <file.desktop> [ICON <icon.svg>])
#
# Declares a SHADOW QML application: an executable with its QML compiled in,
# linked against the shared application runtime, plus its launcher and icon.
function(shadow_add_app target)
    cmake_parse_arguments(A "" "URI;DESKTOP;ICON" "SOURCES;QML;RESOURCES;LIBS" ${ARGN})
    qt_add_executable(${target} ${A_SOURCES})
    qt_add_qml_module(${target}
        URI ${A_URI}
        VERSION 1.0
        NO_PLUGIN
        QML_FILES ${A_QML}
        RESOURCES ${A_RESOURCES}
        DEPENDENCIES QtQuick
    )
    target_link_libraries(${target} PRIVATE Shadow::App ${A_LIBS})
    install(TARGETS ${target} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(A_DESKTOP)
        install(FILES ${A_DESKTOP} DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    endif()
    if(A_ICON)
        install(FILES ${A_ICON} DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    endif()
endfunction()
