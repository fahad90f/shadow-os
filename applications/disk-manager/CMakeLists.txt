# SHADOW Disk Manager — drives, partitions, file systems and disk images
# through UDisks2.
shadow_add_app(shadow-disks
    URI org.shadow.diskmanager
    SOURCES
        main.cpp
        udisksclient.cpp udisksclient.h
        imageworker.cpp imageworker.h
        systemdevices.cpp systemdevices.h
    QML Main.qml DestructiveDialog.qml ActionDialog.qml
    DESKTOP org.shadow.diskmanager.desktop
    LIBS Qt6::DBus
)

# Test driver (not installed): runs single back-end actions from the command
# line, used by tests/run-tests.sh against loop devices.
qt_add_executable(shadow-disks-testctl
    tests/testctl.cpp
    udisksclient.cpp udisksclient.h
    imageworker.cpp imageworker.h
    systemdevices.cpp systemdevices.h
)
target_link_libraries(shadow-disks-testctl PRIVATE Qt6::Core Qt6::DBus)
