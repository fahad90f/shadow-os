# SHADOW Monitor — system monitor and task manager.
#
# One binary, two launchers: "System Monitor" opens the overview, "Task
# Manager" opens the same application on its Processes page.
shadow_add_app(shadow-monitor
    URI org.shadow.monitor
    SOURCES
        main.cpp
        sysfs.cpp sysfs.h
        jobthread.cpp jobthread.h
        cpumemory.cpp cpumemory.h
        storage.cpp storage.h
        network.cpp network.h
        sensors.cpp sensors.h
        gpu.cpp gpu.h
        systemsampler.cpp systemsampler.h
        systemmonitor.cpp systemmonitor.h
        processreader.cpp processreader.h
        processmodel.cpp processmodel.h
        servicemodel.cpp servicemodel.h
        exposurewatcher.cpp exposurewatcher.h
    QML
        Main.qml
        OverviewPage.qml
        ProcessesPage.qml
        ServicesPage.qml
        DisksPage.qml
        NetworkPage.qml
        SensorsPage.qml
        HistoryCard.qml
        SeriesChart.qml
        GaugeTile.qml
        Figure.qml
        ListRow.qml
    DESKTOP org.shadow.monitor.desktop
    LIBS Qt6::DBus
)

install(FILES org.shadow.taskmanager.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
