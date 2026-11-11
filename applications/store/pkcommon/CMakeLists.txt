# nexus-pkcommon — PackageKit and APT/dpkg plumbing shared by NEXUS Store and
# NEXUS Updater (static library; see PkCommon.cmake for how the apps use it).
qt_add_library(nexus-pkcommon STATIC
    pkutil.cpp pkutil.h
    pkservice.cpp pkservice.h
    pkquery.cpp pkquery.h
    pkjob.cpp pkjob.h
    pkhistory.cpp pkhistory.h
    packagemodel.cpp packagemodel.h
    updatechecker.cpp updatechecker.h
    systemfacts.cpp systemfacts.h
    asyncrun.h
)
target_include_directories(nexus-pkcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(nexus-pkcommon PUBLIC PK::packagekitqt6 Qt6::Core Qt6::DBus Qt6::Concurrent Nexus::App)

# Terminal driver for the same Job/Query code the applications use — for the
# VM integration tests and for debugging transactions. Never installed.
option(NEXUS_PKCOMMON_TOOLS "Build nexus-pkcommon-cli (developer/test tool, not installed)" OFF)
if(NEXUS_PKCOMMON_TOOLS)
    qt_add_executable(nexus-pkcommon-cli tools/pkcommon-cli.cpp)
    target_link_libraries(nexus-pkcommon-cli PRIVATE nexus-pkcommon)
    set_target_properties(nexus-pkcommon-cli PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
