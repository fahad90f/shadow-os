// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Test page: a long NxPage. The header with its actions stays visible while
// the content scrolls.
NxWindow {
    id: window
    title: "ShadowUI Gallery — page"
    width: 1000
    height: 640

    property int applied: 0

    NxPage {
        id: page
        anchors.fill: parent
        title: "Proxy settings"
        subtitle: "A page longer than the window: “Apply” must stay reachable while its content scrolls."
        actions: [
            NxButton { text: "Discard" },
            NxButton { id: apply; text: "Apply"; kind: "primary"; iconName: "check"; onClicked: window.applied++ }
        ]

        Repeater {
            model: 6
            NxSection {
                id: section
                required property int index
                title: "Section " + (index + 1)
                NxRow { title: "Host"; TextField { width: 220; placeholderText: "proxy.example.org" } }
                NxRow { title: "Port"; SpinBox { from: 1; to: 65535; value: 3128 + section.index } }
                NxRow { title: "Use for all protocols"; Switch { } }
            }
        }
        Label { id: lastItem; text: "End of the page" }
    }

    Component.onCompleted: if (shadowApp.arguments.indexOf("scrolled") >= 0) Qt.callLater(() => page.flickable.contentY = 420)

    CaseChecks {
        name: "page"
        interval: 250
        steps: [
            () => {
                verify(page.flickable.contentHeight > page.flickable.height, "the content is longer than the view")
                page.flickable.contentY = page.flickable.contentHeight - page.flickable.height
            },
            () => {
                const top = apply.mapToItem(null, 0, 0)
                verify(top.y >= 0 && top.y + apply.height <= window.height, "Apply is inside the window after scrolling to the end (y " + Math.round(top.y) + ")")
                const end = lastItem.mapToItem(null, 0, 0)
                verify(end.y + lastItem.height <= window.height, "the last item of the content is reachable")
                verify(end.y > apply.mapToItem(null, 0, 0).y + apply.height, "content does not overlap the header")
                apply.clicked()
                compare(window.applied, 1, "Apply works while scrolled")
            }
        ]
    }
}
