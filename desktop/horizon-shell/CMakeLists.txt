# horizon-shell — the Shadow Horizon desktop shell (panel, dock, launcher,
# wallpaper, session menu, authentication agent) and the session that starts
# it on KWin (Wayland).
find_package(Qt6 REQUIRED COMPONENTS WaylandClient)
find_package(PlasmaWaylandProtocols REQUIRED)
find_package(KF6Service REQUIRED)
find_package(KF6KIO REQUIRED)
find_package(KF6WindowSystem REQUIRED)
find_package(KF6GlobalAccel REQUIRED)
find_package(PolkitQt6-1 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

# Core QML plus the QML of every shell module (modules/<name>/qml/). All of
# it forms one QML module, org.shadow.horizon, so file names must be unique.
#
# HORIZON_MODULES restricts the build to the named modules ("-" for none), so
# a module can be developed while others are unfinished. Default: all.
set(HORIZON_MODULES "" CACHE STRING "Semicolon-separated shell modules to build (default: all; '-' for none)")
file(GLOB HORIZON_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} qml/*.qml)
file(GLOB _module_dirs CONFIGURE_DEPENDS LIST_DIRECTORIES true modules/*)
set(HORIZON_MODULE_SOURCES "")
set(HORIZON_MODULE_DIRS "")
set(HORIZON_MODULE_CMAKE "")
foreach(dir IN LISTS _module_dirs)
    if(NOT IS_DIRECTORY ${dir})
        continue()
    endif()
    get_filename_component(_module ${dir} NAME)
    if(HORIZON_MODULES AND NOT _module IN_LIST HORIZON_MODULES)
        continue()
    endif()
    message(STATUS "horizon-shell module: ${_module}")
    list(APPEND HORIZON_MODULE_DIRS ${dir})
    file(GLOB _sources CONFIGURE_DEPENDS ${dir}/*.cpp ${dir}/*.h)
    list(APPEND HORIZON_MODULE_SOURCES ${_sources})
    file(GLOB _qml CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${dir}/qml/*.qml)
    list(APPEND HORIZON_QML ${_qml})
    if(EXISTS ${dir}/module.cmake)
        list(APPEND HORIZON_MODULE_CMAKE ${dir}/module.cmake)
    endif()
endforeach()
foreach(f IN LISTS HORIZON_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()

qt_add_executable(horizon-shell
    src/main.cpp
    src/applications.cpp src/applications.h
    src/tasks.cpp src/tasks.h
    src/plasmawindowmanagement.cpp src/plasmawindowmanagement.h
    src/shellsettings.cpp src/shellsettings.h
    src/workspaces.cpp src/workspaces.h
    src/session.cpp src/session.h
    src/shortcuts.cpp src/shortcuts.h
    src/polkitagent.cpp src/polkitagent.h
    src/effects.cpp src/effects.h
    src/module.cpp src/module.h
    ${HORIZON_MODULE_SOURCES}
)
qt6_generate_wayland_protocol_client_sources(horizon-shell
    FILES ${PLASMA_WAYLAND_PROTOCOLS_DIR}/plasma-window-management.xml)
qt_add_qml_module(horizon-shell
    URI org.shadow.horizon
    VERSION 1.0
    NO_PLUGIN
    QML_FILES ${HORIZON_QML}
    DEPENDENCIES QtQuick
)
target_include_directories(horizon-shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
foreach(dir IN LISTS HORIZON_MODULE_DIRS)
    target_include_directories(horizon-shell PRIVATE ${dir})
endforeach()
target_link_libraries(horizon-shell PRIVATE
    Shadow::App Qt6::WaylandClient PkgConfig::WAYLAND_CLIENT
    KF6::Service KF6::KIOGui KF6::WindowSystem KF6::GlobalAccel
    PolkitQt6-1::Agent PolkitQt6-1::Core)

install(TARGETS horizon-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- the session --------------------------------------------------------------------
# /usr/libexec/shadow is hard-wired in the session scripts.
install(PROGRAMS session/shadow-horizon-session DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS session/horizon-session-leader DESTINATION ${SHADOW_LIBEXEC_DIR})
install(FILES session/shadow-horizon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)
install(FILES session/org.shadow.horizon.shell.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES session/shadow-portals.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)
install(FILES session/shadow-horizon-session.target DESTINATION lib/systemd/user)
# System-wide defaults; a user's own files in ~/.config take precedence.
install(FILES session/shadow-applications.menu DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/menus)
install(FILES session/kwinrc session/kscreenlockerrc DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg)

# Per-module build additions (extra packages, link libraries, generated
# sources). The target "horizon-shell" exists at this point.
foreach(module_cmake IN LISTS HORIZON_MODULE_CMAKE)
    include(${module_cmake})
endforeach()
