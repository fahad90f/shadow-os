# SHADOW System Information — hardware and software summary.
# This is also the reference implementation of a SHADOW application; see
# docs/applications/development.md.
shadow_add_app(shadow-sysinfo
    URI org.shadow.sysinfo
    SOURCES main.cpp systeminfo.cpp systeminfo.h
    QML Main.qml
    DESKTOP org.shadow.sysinfo.desktop
)
