// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.nexus.ui
import org.nexus.files

// The content of one tab: a folder pane, or two side by side (split view).
Item {
    id: page

    required property var shell
    // Folder shown when the tab is created.
    required property url initialUrl
    // A file to select once the folder is listed (--select); empty for none.
    required property url initialSelection

    property bool split: false
    property int activeSide: 0

    readonly property FolderPane leftPane: left
    readonly property FolderPane rightPane: rightLoader.item as FolderPane
    readonly property FolderPane activePane: split && activeSide === 1 && rightPane ? rightPane : left
    readonly property FolderPane otherPane: !split || !rightPane ? null : (activeSide === 1 ? left : rightPane)
    readonly property string title: activePane.folder.title
    readonly property string symbol: activePane.folder.symbol

    function setSplit(enabled) {
        if (enabled === page.split) return
        if (enabled) {
            page.split = true
        } else {
            // Keep the folder the user is looking at.
            if (page.activeSide === 1 && page.rightPane)
                left.folder.open(page.rightPane.folder.url)
            page.activeSide = 0
            page.split = false
            left.focusView()
        }
    }

    function switchPane() {
        if (!page.split || !page.rightPane) return
        page.activeSide = page.activeSide === 0 ? 1 : 0
        page.activePane.focusView()
    }

    Component.onCompleted: {
        if (page.initialSelection.toString() !== "")
            left.folder.reveal(page.initialSelection)
        else
            left.folder.open(page.initialUrl)
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        FolderPane {
            id: left
            shell: page.shell
            active: page.activeSide === 0 || !page.split
            showHeader: page.split
            SplitView.fillWidth: true
            SplitView.minimumWidth: 260
            focus: page.activeSide === 0 || !page.split
            onActivated: page.activeSide = 0
        }

        Loader {
            id: rightLoader
            active: page.split
            visible: active
            SplitView.preferredWidth: page.width / 2
            SplitView.minimumWidth: 260
            focus: page.split && page.activeSide === 1
            sourceComponent: FolderPane {
                shell: page.shell
                active: page.activeSide === 1
                showHeader: true
                focus: true
                onActivated: page.activeSide = 1
                Component.onCompleted: {
                    // The second pane starts in the same folder as the first.
                    folder.open(left.folder.url)
                    page.activeSide = 1
                    focusView()
                }
            }
        }
    }
}
