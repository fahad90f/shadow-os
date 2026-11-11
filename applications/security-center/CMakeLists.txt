# NEXUS Security Center — the real security state of this computer and the
# host firewall, on top of nexus-security-status and nexus-firewall
# (security/audit, security/firewall; package nexus-hardening).
file(GLOB SECCENTER_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.qml *.js)

nexus_add_app(nexus-security-center
    URI org.nexus.securitycenter
    SOURCES main.cpp securitycenter.cpp securitycenter.h
    QML ${SECCENTER_QML}
    DESKTOP org.nexus.securitycenter.desktop
)
