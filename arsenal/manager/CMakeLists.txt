# NEXUS Arsenal — security tool profile manager (package nexus-arsenal).
#
# The profile manifests in ../profiles are data: they are owned by the
# architecture-independent package nexus-arsenal-data, which debian/ assembles
# from ../packaging/nexus-arsenal-data.install. The install() rule below puts
# the same files into the CMake install component "nexus-arsenal-data", so a
# plain "cmake --install" (developer trees, tests) yields a working layout too;
# the nexus-arsenal binary package itself never contains them.
find_package(PackageKitQt6 REQUIRED)

file(GLOB ARSENAL_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml components/*.qml)

nexus_add_app(nexus-arsenal
    URI org.nexus.arsenal
    SOURCES
        main.cpp
        arsenal.cpp arsenal.h
        manifest.cpp manifest.h
        packagedb.cpp packagedb.h
        pkjob.cpp pkjob.h
    QML Main.qml ${ARSENAL_QML}
    DESKTOP org.nexus.arsenal.desktop
    LIBS PK::packagekitqt6 Qt6::DBus
)
set_target_properties(nexus-arsenal PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

file(GLOB ARSENAL_MANIFESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../profiles/*.json)
install(FILES ${ARSENAL_MANIFESTS}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/nexus/arsenal/profiles
        COMPONENT nexus-arsenal-data)

# User documentation, next to the application (package nexus-arsenal).
file(GLOB ARSENAL_DOCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../docs/*.md)
install(FILES ${ARSENAL_DOCS} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/nexus-arsenal)
