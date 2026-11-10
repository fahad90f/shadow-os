# horizon-shell — the NEXUS HORIZON desktop shell (panel, dock, launcher,
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

file(GLOB HORIZON_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} qml/*.qml)
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
)
qt6_generate_wayland_protocol_client_sources(horizon-shell
    FILES ${PLASMA_WAYLAND_PROTOCOLS_DIR}/plasma-window-management.xml)
qt_add_qml_module(horizon-shell
    URI org.nexus.horizon
    VERSION 1.0
    NO_PLUGIN
    QML_FILES ${HORIZON_QML}
    DEPENDENCIES QtQuick
)
target_include_directories(horizon-shell PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(horizon-shell PRIVATE
    Nexus::App Qt6::WaylandClient PkgConfig::WAYLAND_CLIENT
    KF6::Service KF6::KIOGui KF6::WindowSystem KF6::GlobalAccel
    PolkitQt6-1::Agent PolkitQt6-1::Core)

install(TARGETS horizon-shell RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
