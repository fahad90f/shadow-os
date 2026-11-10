# NexusUI — the shared UI foundation of NEXUS HORIZON and every NEXUS app.
#
#   org.nexus.ui     QML module: Theme (generated from the design tokens),
#                    Appearance, icons and the Nx* component library
#   org.nexus.style  Qt Quick Controls style ("NEXUS")
#   Nexus::App       static application runtime linked into each app

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Svg Widgets DBus Qml)

# ---- Theme.qml generated from the design tokens --------------------------------
set(NEXUS_TOKENS "${CMAKE_SOURCE_DIR}/desktop/themes/tokens/nexus-tokens.json")
set(NEXUS_GEN_TOKENS "${CMAKE_SOURCE_DIR}/scripts/gen-tokens.py")
set(NEXUS_THEME_QML "${CMAKE_CURRENT_BINARY_DIR}/generated/Theme.qml")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${NEXUS_GEN_TOKENS} qml ${NEXUS_THEME_QML}
    RESULT_VARIABLE _tokens_rc)
if(_tokens_rc)
    message(FATAL_ERROR "design token generation failed (scripts/gen-tokens.py)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${NEXUS_TOKENS} ${NEXUS_GEN_TOKENS})

# ---- org.nexus.ui ---------------------------------------------------------------
file(GLOB NEXUS_UI_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} qml/*.qml)
foreach(f IN LISTS NEXUS_UI_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()
set_source_files_properties(${NEXUS_THEME_QML} PROPERTIES
    QT_RESOURCE_ALIAS Theme.qml
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(nexusui SHARED)
set_target_properties(nexusui PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
qt_add_qml_module(nexusui
    URI org.nexus.ui
    VERSION 1.0
    SOURCES
        src/appearance.cpp src/appearance.h
        src/iconprovider.cpp src/iconprovider.h
    QML_FILES ${NEXUS_THEME_QML} ${NEXUS_UI_QML}
    DEPENDENCIES QtQuick QtQuick.Controls QtQuick.Layouts QtQuick.Shapes
)

file(STRINGS icons/icons.list NEXUS_ICON_FILES)
list(TRANSFORM NEXUS_ICON_FILES PREPEND icons/)
qt_add_resources(nexusui nexus_symbolic_icons
    PREFIX /nexus/icons
    BASE icons
    FILES ${NEXUS_ICON_FILES})

target_include_directories(nexusui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(nexusui PUBLIC Qt6::Quick Qt6::Svg)

# ---- org.nexus.style ------------------------------------------------------------
file(GLOB NEXUS_STYLE_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} style/*.qml)
foreach(f IN LISTS NEXUS_STYLE_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()

qt_add_library(nexusstyle SHARED)
set_target_properties(nexusstyle PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
qt_add_qml_module(nexusstyle
    URI org.nexus.style
    VERSION 1.0
    QML_FILES ${NEXUS_STYLE_QML}
    IMPORTS QtQuick.Controls.Basic
    DEPENDENCIES QtQuick QtQuick.Templates org.nexus.ui
)
target_link_libraries(nexusstyle PRIVATE Qt6::Quick Qt6::QuickControls2)
add_dependencies(nexusstyle nexusui)

# ---- Nexus::App -----------------------------------------------------------------
qt_add_library(nexusapp STATIC
    src/app/nexusapp.cpp src/app/nexusapp.h
    src/app/command.cpp src/app/command.h)
add_library(Nexus::App ALIAS nexusapp)
target_include_directories(nexusapp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(nexusapp PUBLIC
    Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::DBus Qt6::QuickControls2 nexusui)
add_dependencies(nexusapp nexusstyle nexusuiplugin nexusstyleplugin)

# ---- install ----------------------------------------------------------------------
install(TARGETS nexusui nexusstyle LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP)
foreach(mod ui style)
    install(TARGETS nexus${mod}plugin LIBRARY DESTINATION ${NEXUS_QML_INSTALL_DIR}/org/nexus/${mod})
    install(FILES
        ${NEXUS_QML_BUILD_DIR}/org/nexus/${mod}/qmldir
        ${NEXUS_QML_BUILD_DIR}/org/nexus/${mod}/nexus${mod}.qmltypes
        DESTINATION ${NEXUS_QML_INSTALL_DIR}/org/nexus/${mod})
endforeach()
