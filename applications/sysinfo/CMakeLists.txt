# NEXUS System Information — hardware and software summary.
# This is also the reference implementation of a NEXUS application; see
# docs/applications/development.md.
nexus_add_app(nexus-sysinfo
    URI org.nexus.sysinfo
    SOURCES main.cpp systeminfo.cpp systeminfo.h
    QML Main.qml
    DESKTOP org.nexus.sysinfo.desktop
)
