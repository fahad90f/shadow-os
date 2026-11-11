# SHADOW Files — graphical file manager built on KIO (file operations, remote
# protocols, trash, previews) and Solid (storage devices).
find_package(KF6KIO REQUIRED)
find_package(KF6Solid REQUIRED)
find_package(KF6CoreAddons REQUIRED)
find_package(KF6Service REQUIRED)
find_package(KF6Config REQUIRED)

file(GLOB FILES_SOURCES CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/*.cpp src/*.h)
file(GLOB FILES_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} qml/*.qml)
foreach(f IN LISTS FILES_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()

shadow_add_app(shadow-files
    URI org.shadow.files
    SOURCES main.cpp ${FILES_SOURCES}
    QML Main.qml ${FILES_QML}
    DESKTOP org.shadow.files.desktop
    LIBS KF6::KIOCore KF6::KIOGui KF6::KIOWidgets KF6::Solid KF6::CoreAddons
         KF6::Service KF6::ConfigCore Qt6::Concurrent
)
target_include_directories(shadow-files PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# src/svgiconengine.cpp is a Qt plugin compiled into the executable.
target_compile_definitions(shadow-files PRIVATE QT_STATICPLUGIN)
