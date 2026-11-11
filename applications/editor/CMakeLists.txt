# SHADOW Text Editor — programmer's text editor (tabs, syntax highlighting
# through KSyntaxHighlighting, find/replace, safe encoding handling).
find_package(KF6SyntaxHighlighting REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Concurrent)

shadow_add_app(shadow-editor
    URI org.shadow.editor
    SOURCES main.cpp document.cpp document.h textfile.cpp textfile.h backend.cpp backend.h
            codeeditor.cpp codeeditor.h
    QML Main.qml EditorView.qml FindBar.qml FileNotice.qml PathPicker.qml
    RESOURCES themes/shadow-dark.theme themes/shadow-light.theme
    DESKTOP org.shadow.editor.desktop
    LIBS KF6::SyntaxHighlighting Qt6::Concurrent Qt6::Widgets
)
