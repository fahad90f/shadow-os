# NEXUS Store — graphical software store: AppStream catalogue, PackageKit
# transactions. The PackageKit/APT plumbing in pkcommon/ is shared with
# NEXUS Updater (applications/updater).
include(${CMAKE_CURRENT_SOURCE_DIR}/pkcommon/PkCommon.cmake)
nexus_pkcommon_qml(STORE_SHARED_QML)

find_package(AppStreamQt 1.0 REQUIRED)

file(GLOB STORE_PAGES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml components/*.qml)

nexus_add_app(nexus-store
    URI org.nexus.store
    SOURCES
        main.cpp
        store.cpp store.h
        catalog.cpp catalog.h
        packageinfo.cpp packageinfo.h
        remoteimage.cpp remoteimage.h
    QML Main.qml ${STORE_PAGES} ${STORE_SHARED_QML}
    DESKTOP org.nexus.store.desktop
    LIBS nexus-pkcommon AppStreamQt Qt6::Concurrent Qt6::Network
)

# Qt's QML import scan only matters when QML plugins are linked statically; with
# the shared Qt of the distribution it merely produces "link target does not
# exist" warnings at configure time.
set_target_properties(nexus-store PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
