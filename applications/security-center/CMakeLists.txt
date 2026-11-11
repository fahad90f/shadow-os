# SHADOW Security Center — the real security state of this computer and the
# host firewall, on top of shadow-security-status and shadow-firewall
# (security/audit, security/firewall; package shadow-hardening).
file(GLOB SECCENTER_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.qml *.js)

shadow_add_app(shadow-security-center
    URI org.shadow.securitycenter
    SOURCES main.cpp securitycenter.cpp securitycenter.h
    QML ${SECCENTER_QML}
    DESKTOP org.shadow.securitycenter.desktop
)
