# NEXUS Text Editor — programmer's text editor (tabs, syntax highlighting
# through KSyntaxHighlighting, find/replace, safe encoding handling).
find_package(KF6SyntaxHighlighting REQUIRED)
find_package(Qt6 REQUIRED COMPONENTS Concurrent)

nexus_add_app(nexus-editor
    URI org.nexus.editor
    SOURCES main.cpp document.cpp document.h textfile.cpp textfile.h backend.cpp backend.h
    QML Main.qml EditorView.qml FindBar.qml FileNotice.qml PathPicker.qml
    RESOURCES themes/nexus-dark.theme themes/nexus-light.theme
    DESKTOP org.nexus.editor.desktop
    LIBS KF6::SyntaxHighlighting Qt6::Concurrent
)
