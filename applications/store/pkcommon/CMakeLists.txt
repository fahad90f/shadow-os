# shadow-pkcommon — PackageKit and APT/dpkg plumbing shared by SHADOW Store and
# SHADOW Updater (static library; see PkCommon.cmake for how the apps use it).
qt_add_library(shadow-pkcommon STATIC
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
target_include_directories(shadow-pkcommon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(shadow-pkcommon PUBLIC PK::packagekitqt6 Qt6::Core Qt6::DBus Qt6::Concurrent Shadow::App)

# Terminal driver for the same Job/Query code the applications use — for the
# VM integration tests and for debugging transactions. Never installed.
option(SHADOW_PKCOMMON_TOOLS "Build shadow-pkcommon-cli (developer/test tool, not installed)" OFF)
if(SHADOW_PKCOMMON_TOOLS)
    qt_add_executable(shadow-pkcommon-cli tools/pkcommon-cli.cpp)
    target_link_libraries(shadow-pkcommon-cli PRIVATE shadow-pkcommon)
    set_target_properties(shadow-pkcommon-cli PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()
