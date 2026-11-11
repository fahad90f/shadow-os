# ShadowUI — the shared UI foundation of Shadow Horizon and every SHADOW app.
#
#   org.shadow.ui     QML module: Theme (generated from the design tokens),
#                    Appearance, icons and the Nx* component library
#   org.shadow.style  Qt Quick Controls style ("SHADOW")
#   Shadow::App       static application runtime linked into each app

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Svg Widgets DBus Qml)

# ---- Theme.qml generated from the design tokens --------------------------------
set(SHADOW_TOKENS "${CMAKE_SOURCE_DIR}/desktop/themes/tokens/shadow-tokens.json")
set(SHADOW_GEN_TOKENS "${CMAKE_SOURCE_DIR}/scripts/gen-tokens.py")
set(SHADOW_THEME_QML "${CMAKE_CURRENT_BINARY_DIR}/generated/Theme.qml")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${SHADOW_GEN_TOKENS} qml ${SHADOW_THEME_QML}
    RESULT_VARIABLE _tokens_rc)
if(_tokens_rc)
    message(FATAL_ERROR "design token generation failed (scripts/gen-tokens.py)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SHADOW_TOKENS} ${SHADOW_GEN_TOKENS})

# ---- org.shadow.ui ---------------------------------------------------------------
file(GLOB SHADOW_UI_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} qml/*.qml)
foreach(f IN LISTS SHADOW_UI_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()
set_source_files_properties(${SHADOW_THEME_QML} PROPERTIES
    QT_RESOURCE_ALIAS Theme.qml
    QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(shadowui SHARED)
set_target_properties(shadowui PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
qt_add_qml_module(shadowui
    URI org.shadow.ui
    VERSION 1.0
    SOURCES
        src/appearance.cpp src/appearance.h
        src/iconprovider.cpp src/iconprovider.h
    QML_FILES ${SHADOW_THEME_QML} ${SHADOW_UI_QML}
    DEPENDENCIES QtQuick QtQuick.Controls QtQuick.Layouts QtQuick.Shapes
)

file(STRINGS icons/icons.list SHADOW_ICON_FILES)
list(TRANSFORM SHADOW_ICON_FILES PREPEND icons/)
qt_add_resources(shadowui shadow_symbolic_icons
    PREFIX /shadow/icons
    BASE icons
    FILES ${SHADOW_ICON_FILES})

target_include_directories(shadowui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(shadowui PUBLIC Qt6::Quick Qt6::Svg)

# ---- org.shadow.style ------------------------------------------------------------
file(GLOB SHADOW_STYLE_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} style/*.qml)
foreach(f IN LISTS SHADOW_STYLE_QML)
    get_filename_component(_name ${f} NAME)
    set_source_files_properties(${f} PROPERTIES QT_RESOURCE_ALIAS ${_name})
endforeach()

qt_add_library(shadowstyle SHARED)
set_target_properties(shadowstyle PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0)
qt_add_qml_module(shadowstyle
    URI org.shadow.style
    VERSION 1.0
    QML_FILES ${SHADOW_STYLE_QML}
    IMPORTS QtQuick.Controls.Basic
    DEPENDENCIES QtQuick QtQuick.Templates org.shadow.ui
)
target_link_libraries(shadowstyle PRIVATE Qt6::Quick Qt6::QuickControls2)

# SHADOW look for Qt Quick Dialogs (FileDialog, FolderDialog). The Qt Quick
# implementation of those dialogs loads its QML from Qt's own resource
# directory through a file selector named after the active Controls style, so
# files placed in "+org.shadow.style" below that directory replace the built-in
# ones whenever org.shadow.style is the style. They are deliberately not part
# of the org.shadow.style QML module (their names would become style types).
qt_add_resources(shadowstyle shadow_quick_dialogs
    PREFIX "/qt-project.org/imports/QtQuick/Dialogs/quickimpl/qml/+org.shadow.style"
    BASE style/dialogs
    FILES
        style/dialogs/FileDialog.qml
        style/dialogs/FolderDialog.qml
        style/dialogs/NxFileRow.qml
        style/dialogs/NxPathBar.qml)
add_dependencies(shadowstyle shadowui)

# ---- Shadow::App -----------------------------------------------------------------
qt_add_library(shadowapp STATIC
    src/app/shadowapp.cpp src/app/shadowapp.h
    src/app/command.cpp src/app/command.h)
add_library(Shadow::App ALIAS shadowapp)
target_include_directories(shadowapp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src/app)
target_link_libraries(shadowapp PUBLIC
    Qt6::Widgets Qt6::Quick Qt6::Qml Qt6::DBus Qt6::QuickControls2 shadowui)
add_dependencies(shadowapp shadowstyle shadowuiplugin shadowstyleplugin)

# ---- install ----------------------------------------------------------------------
install(TARGETS shadowui shadowstyle LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} NAMELINK_SKIP)
foreach(mod ui style)
    install(TARGETS shadow${mod}plugin LIBRARY DESTINATION ${SHADOW_QML_INSTALL_DIR}/org/shadow/${mod})
    install(FILES
        ${SHADOW_QML_BUILD_DIR}/org/shadow/${mod}/qmldir
        ${SHADOW_QML_BUILD_DIR}/org/shadow/${mod}/shadow${mod}.qmltypes
        DESTINATION ${SHADOW_QML_INSTALL_DIR}/org/shadow/${mod})
endforeach()
