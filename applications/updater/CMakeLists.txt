# SHADOW Updater — system updates, update history, restart notifications and
# package-system recovery on top of PackageKit. Shares its PackageKit/APT
# plumbing with SHADOW Store (applications/store/pkcommon).
include(${CMAKE_CURRENT_SOURCE_DIR}/../store/pkcommon/PkCommon.cmake)
shadow_pkcommon_qml(UPDATER_SHARED_QML)

file(GLOB UPDATER_PAGES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml)

shadow_add_app(shadow-updater
    URI org.shadow.updater
    SOURCES main.cpp updater.cpp updater.h
    QML Main.qml ${UPDATER_PAGES} ${UPDATER_SHARED_QML}
    DESKTOP org.shadow.updater.desktop
    LIBS shadow-pkcommon Qt6::Concurrent
)

# Qt's QML import scan only matters when QML plugins are linked statically; with
# the shared Qt of the distribution it merely produces "link target does not
# exist" warnings at configure time.
set_target_properties(shadow-updater PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
