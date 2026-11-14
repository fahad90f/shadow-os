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

# Install updates during the next restart (systemd.offline-updates(7)).
install(PROGRAMS offline/shadow-offline-update DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(FILES offline/shadow-offline-update.service DESTINATION lib/systemd/system)
install(FILES offline/packagekit-offline-update.conf
        DESTINATION lib/systemd/system/packagekit-offline-update.service.d
        RENAME 50-shadow.conf)
# Pulled in by every update boot; the unit's condition and the tool itself
# check that the update is one this tool prepared.
install(CODE "
    set(wants \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/systemd/system/system-update.target.wants\")
    file(MAKE_DIRECTORY \"\${wants}\")
    file(CREATE_LINK ../shadow-offline-update.service \"\${wants}/shadow-offline-update.service\" SYMBOLIC)
")
