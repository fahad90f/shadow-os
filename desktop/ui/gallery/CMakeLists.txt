# Component gallery: renders every NexusUI component in one window. Used for
# visual review of the design system and by the GUI smoke tests; not installed.
# The Case*.qml pages are focused test pages (nexus-ui-gallery --case <name>).
qt_add_executable(nexus-ui-gallery main.cpp)
qt_add_qml_module(nexus-ui-gallery
    URI org.nexus.gallery
    VERSION 1.0
    NO_PLUGIN
    QML_FILES
        Main.qml
        CaseChecks.qml
        CaseDialogs.qml
        CaseSegmented.qml
        CaseRows.qml
        CaseTable.qml
        CaseToast.qml
        CasePage.qml
        CaseMotion.qml
        CaseGlyphs.qml
)
target_link_libraries(nexus-ui-gallery PRIVATE Nexus::App)
