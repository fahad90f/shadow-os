# SHADOW Network Center — network status, configuration and diagnostics.
#
# Pages and shared QML pieces are globbed, so adding one does not touch this
# file. The privileged helper is a Python script (standard library only)
# installed to ${SHADOW_LIBEXEC_DIR}; its polkit policy is generated so that
# the path it authorises is always the path the helper is installed to.
file(GLOB NC_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} pages/*.qml components/*.qml)

cmake_path(ABSOLUTE_PATH SHADOW_LIBEXEC_DIR BASE_DIRECTORY "${CMAKE_INSTALL_PREFIX}" NORMALIZE
           OUTPUT_VARIABLE NC_LIBEXEC_FULL)
set(NC_HELPER_PATH "${NC_LIBEXEC_FULL}/network-helper")

shadow_add_app(shadow-network-center
    URI org.shadow.networkcenter
    SOURCES
        main.cpp
        netutil.cpp netutil.h
        recordmodel.cpp recordmodel.h
        netstate.cpp netstate.h
        nmclient.cpp nmclient.h
        sockets.cpp sockets.h
        diagnostics.cpp diagnostics.h
        capturestream.cpp capturestream.h
        capture.cpp capture.h
    QML Main.qml ${NC_QML}
    DESKTOP org.shadow.networkcenter.desktop
    LIBS Qt6::Network Qt6::DBus
)
target_compile_definitions(shadow-network-center PRIVATE SHADOW_NETWORK_HELPER="${NC_HELPER_PATH}")

# Privileged helper and its polkit actions (docs/applications/development.md §4).
install(PROGRAMS network-helper DESTINATION ${SHADOW_LIBEXEC_DIR})
configure_file(org.shadow.networkcenter.policy.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.shadow.networkcenter.policy @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.shadow.networkcenter.policy
        DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)

# A copy next to the build tree's bin/, so an uninstalled binary finds it
# (see Net::helperPath()).
configure_file(network-helper ${CMAKE_BINARY_DIR}/libexec/shadow/network-helper COPYONLY)
