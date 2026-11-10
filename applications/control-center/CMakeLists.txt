# NEXUS Control Center — unified system settings.
#
# Pages are self-contained: a QML file in pages/ plus (optionally) a back-end
# class in backends/ that is a QML_ELEMENT of this module. Both directories
# are globbed, so adding a page never touches this file — only the page table
# in Main.qml.
file(GLOB CC_BACKENDS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} backends/*.cpp backends/*.h)
file(GLOB CC_PAGES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml)
file(GLOB CC_HELPERS CONFIGURE_DEPENDS helpers/*-helper)
file(GLOB CC_POLICIES CONFIGURE_DEPENDS helpers/*.policy)

find_package(KF6BluezQt QUIET)
find_package(KF6PulseAudioQt QUIET)
find_package(KF6Config QUIET)

nexus_add_app(nexus-control-center
    URI org.nexus.controlcenter
    SOURCES main.cpp dumpregistry.cpp dumpregistry.h ${CC_BACKENDS}
    QML Main.qml ${CC_PAGES}
    DESKTOP org.nexus.controlcenter.desktop
)
target_include_directories(nexus-control-center PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/backends)
if(TARGET KF6::BluezQt)
    target_link_libraries(nexus-control-center PRIVATE KF6::BluezQt)
    target_compile_definitions(nexus-control-center PRIVATE NEXUS_HAVE_BLUEZQT)
endif()
if(TARGET KF6::PulseAudioQt)
    target_link_libraries(nexus-control-center PRIVATE KF6::PulseAudioQt)
    target_compile_definitions(nexus-control-center PRIVATE NEXUS_HAVE_PULSEAUDIOQT)
endif()
if(TARGET KF6::ConfigCore)
    target_link_libraries(nexus-control-center PRIVATE KF6::ConfigCore)
    target_compile_definitions(nexus-control-center PRIVATE NEXUS_HAVE_KCONFIG)
endif()

if(CC_HELPERS)
    install(PROGRAMS ${CC_HELPERS} DESTINATION ${NEXUS_LIBEXEC_DIR})
endif()
if(CC_POLICIES)
    install(FILES ${CC_POLICIES} DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
endif()

# "NEXUS Settings" is the same application under the name people search for.
install(FILES org.nexus.settings.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
