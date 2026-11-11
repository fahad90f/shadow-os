# NEXUS Updater — system updates, update history, restart notifications and
# package-system recovery on top of PackageKit. Shares its PackageKit/APT
# plumbing with NEXUS Store (applications/store/pkcommon).
include(${CMAKE_CURRENT_SOURCE_DIR}/../store/pkcommon/PkCommon.cmake)
nexus_pkcommon_qml(UPDATER_SHARED_QML)

file(GLOB UPDATER_PAGES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml)

nexus_add_app(nexus-updater
    URI org.nexus.updater
    SOURCES main.cpp updater.cpp updater.h
    QML Main.qml ${UPDATER_PAGES} ${UPDATER_SHARED_QML}
    DESKTOP org.nexus.updater.desktop
    LIBS nexus-pkcommon Qt6::Concurrent
)

# Qt's QML import scan only matters when QML plugins are linked statically; with
# the shared Qt of the distribution it merely produces "link target does not
# exist" warnings at configure time.
set_target_properties(nexus-updater PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
