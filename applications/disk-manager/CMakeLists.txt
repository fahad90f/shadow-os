# NEXUS Disk Manager — drives, partitions, file systems and disk images
# through UDisks2.
nexus_add_app(nexus-disks
    URI org.nexus.diskmanager
    SOURCES
        main.cpp
        udisksclient.cpp udisksclient.h
        imageworker.cpp imageworker.h
        systemdevices.cpp systemdevices.h
    QML Main.qml DestructiveDialog.qml ActionDialog.qml
    DESKTOP org.nexus.diskmanager.desktop
    LIBS Qt6::DBus
)

# Test driver (not installed): runs single back-end actions from the command
# line, used by tests/run-tests.sh against loop devices.
qt_add_executable(nexus-disks-testctl
    tests/testctl.cpp
    udisksclient.cpp udisksclient.h
    imageworker.cpp imageworker.h
    systemdevices.cpp systemdevices.h
)
target_link_libraries(nexus-disks-testctl PRIVATE Qt6::Core Qt6::DBus)
