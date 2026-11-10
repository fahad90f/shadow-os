# Component gallery: renders every NexusUI component in one window. Used for
# visual review of the design system and by the GUI smoke tests; not installed.
qt_add_executable(nexus-ui-gallery main.cpp)
qt_add_qml_module(nexus-ui-gallery
    URI org.nexus.gallery
    VERSION 1.0
    NO_PLUGIN
    QML_FILES Main.qml
)
target_link_libraries(nexus-ui-gallery PRIVATE Nexus::App)
