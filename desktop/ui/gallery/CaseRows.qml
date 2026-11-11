// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Test page: NxRow draws a divider above itself unless it is the first
// visible row of its parent. Exactly one row per section may be "first".
NxWindow {
    id: window
    title: "ShadowUI Gallery — rows"
    width: 1000
    height: 760

    ListModel {
        id: devices
        ListElement { name: "Alpha" }
        ListElement { name: "Bravo" }
        ListElement { name: "Charlie" }
        ListElement { name: "Delta" }
    }
    property bool showHidden: false
    property int extra: 0

    // Titles of the rows of a section's row container that claim to be first.
    function firstRows(section) {
        const out = []
        const kids = section.rowContainer.children
        for (let i = 0; i < kids.length; i++)
            if (kids[i].visible && kids[i].isFirst === true) out.push(kids[i].title)
        return out.join("|")
    }

    component Probe: NxSection {
        // The rows live in the section's inner column: reach it through a row.
        readonly property Item rowContainer: anchorRow.parent
        property alias anchorRow: anchor
        NxRow { id: anchor; visible: false; title: "(anchor, never visible)" }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceXl

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.spaceLg
            Probe {
                id: moving
                title: "Rows from a model that is reordered"
                headerActions: [
                    NxButton { text: "Move first to end"; onClicked: devices.move(0, devices.count - 1, 1) },
                    NxButton { text: "Add at top"; onClicked: devices.insert(0, { name: "New " + (++window.extra) }) }
                ]
                Repeater {
                    model: devices
                    NxRow {
                        required property string name
                        title: name
                        subtitle: isFirst ? "first — no divider above" : "divider above"
                    }
                }
            }
            Probe {
                id: repeaterFirst
                title: "An empty Repeater before the first row"
                Repeater { model: 0; NxRow { title: "never" } }
                NxRow { title: "Static row A"; subtitle: isFirst ? "first — no divider above" : "divider above" }
                NxRow { title: "Static row B"; subtitle: isFirst ? "first — no divider above" : "divider above" }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Theme.spaceLg
            Probe {
                id: hiddenFirst
                title: "A hidden row before the first visible one"
                headerActions: Switch { text: "Show it"; checked: window.showHidden; onToggled: window.showHidden = checked }
                NxRow { visible: window.showHidden; title: "Sometimes hidden"; subtitle: isFirst ? "first — no divider above" : "divider above" }
                NxRow { title: "Always visible 1"; subtitle: isFirst ? "first — no divider above" : "divider above" }
                NxRow { title: "Always visible 2"; subtitle: isFirst ? "first — no divider above" : "divider above" }
            }
            Probe {
                id: afterContent
                title: "A row after other content keeps its divider"
                Label { text: "A label that is not a row"; padding: Theme.spaceLg }
                NxRow { title: "Row under the label"; subtitle: isFirst ? "first — no divider above" : "divider above" }
            }
        }
    }

    CaseChecks {
        name: "rows"
        steps: [
            () => {
                compare(firstRows(moving), "Alpha", "model rows: Alpha is first")
                compare(firstRows(repeaterFirst), "Static row A", "an empty Repeater before the rows does not count")
                compare(firstRows(hiddenFirst), "Always visible 1", "a hidden row before the first visible one does not count")
                compare(firstRows(afterContent), "", "a row below other visible content is not first")
                devices.move(0, 3, 1)
            },
            () => {
                compare(firstRows(moving), "Bravo", "after moving the first row to the end the new top row is first")
                devices.move(3, 0, 1)
            },
            () => {
                compare(firstRows(moving), "Alpha", "after moving it back")
                devices.insert(0, { name: "Inserted" })
            },
            () => {
                compare(firstRows(moving), "Inserted", "a row inserted at the top becomes the first")
                devices.remove(0)
                window.showHidden = true
            },
            () => {
                compare(firstRows(moving), "Alpha", "after removing the inserted row")
                compare(firstRows(hiddenFirst), "Sometimes hidden", "the row that became visible is now the first")
                window.showHidden = false
            },
            () => compare(firstRows(hiddenFirst), "Always visible 1", "hidden again")
        ]
    }
}
