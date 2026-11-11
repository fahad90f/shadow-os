# SHADOW Arsenal — security tool profile manager (package shadow-arsenal).
#
# The profile manifests in ../profiles are data: they are owned by the
# architecture-independent package shadow-arsenal-data, which debian/ assembles
# from ../packaging/shadow-arsenal-data.install. The install() rule below puts
# the same files into the CMake install component "shadow-arsenal-data", so a
# plain "cmake --install" (developer trees, tests) yields a working layout too;
# the shadow-arsenal binary package itself never contains them.
find_package(PackageKitQt6 REQUIRED)

file(GLOB ARSENAL_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml components/*.qml)

shadow_add_app(shadow-arsenal
    URI org.shadow.arsenal
    SOURCES
        main.cpp
        arsenal.cpp arsenal.h
        manifest.cpp manifest.h
        packagedb.cpp packagedb.h
        pkjob.cpp pkjob.h
    QML Main.qml ${ARSENAL_QML}
    DESKTOP org.shadow.arsenal.desktop
    LIBS PK::packagekitqt6 Qt6::DBus
)
set_target_properties(shadow-arsenal PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)

file(GLOB ARSENAL_MANIFESTS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../profiles/*.json)
install(FILES ${ARSENAL_MANIFESTS}
        DESTINATION ${CMAKE_INSTALL_DATADIR}/shadow/arsenal/profiles
        COMPONENT shadow-arsenal-data)

# User documentation, next to the application (package shadow-arsenal).
file(GLOB ARSENAL_DOCS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../docs/*.md)
install(FILES ${ARSENAL_DOCS} DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/shadow-arsenal)
