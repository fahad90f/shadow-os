# Component gallery: renders every ShadowUI component in one window. Used for
# visual review of the design system and by the GUI smoke tests; not installed.
# The Case*.qml pages are focused test pages (shadow-ui-gallery --case <name>).
qt_add_executable(shadow-ui-gallery main.cpp)
qt_add_qml_module(shadow-ui-gallery
    URI org.shadow.gallery
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
        CaseFiledialog.qml
        CaseFolderdialog.qml
)
target_link_libraries(shadow-ui-gallery PRIVATE Shadow::App)
