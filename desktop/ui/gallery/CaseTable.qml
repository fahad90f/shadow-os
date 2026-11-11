// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Test page: NxDataTable selection. Nothing is selected until the caller or
// the user selects a row; giving the table a model does not select row 0.
NxWindow {
    id: window
    title: "NexusUI Gallery — table"
    width: 1000
    height: 640

    property var columns: [
        { title: "Unit", role: "unit", fill: true },
        { title: "State", role: "state", width: 90 },
    ]
    function rows(n, tag) {
        const out = []
        for (let i = 0; i < n; i++)
            out.push({ unit: tag + "-" + i + ".service", state: i % 3 ? "running" : "exited", mem: 1048576 * (i + 1) * 7 })
        return out
    }
    ListModel { id: listModel }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceMd

        RowLayout {
            spacing: Theme.spaceSm
            NxButton { text: "Replace the models"; onClicked: { late.model = window.rows(6, "new"); preset.model = window.rows(6, "new") } }
            NxButton { text: "Clear selection"; onClicked: late.currentRow = -1 }
            Label {
                Layout.fillWidth: true
                color: Theme.textSecondary
                text: "currentRow — model set later: " + late.currentRow + " · caller selected row 2: " + preset.currentRow
                      + " · item model: " + itemModel.currentRow
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceLg
            NxCard {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: Theme.spaceSm
                NxDataTable { id: late; anchors.fill: parent; columns: window.columns }
            }
            NxCard {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: Theme.spaceSm
                NxDataTable { id: preset; anchors.fill: parent; columns: window.columns; model: window.rows(5, "pre"); currentRow: 2 }
            }
            NxCard {
                Layout.fillWidth: true; Layout.fillHeight: true; padding: Theme.spaceSm
                NxDataTable { id: itemModel; anchors.fill: parent; columns: window.columns; model: listModel }
            }
        }
    }

    Component.onCompleted: {
        // As applications do: the data arrives after the table exists.
        late.model = rows(8, "late")
        for (const r of rows(5, "item")) listModel.append(r)
    }

    CaseChecks {
        name: "table"
        steps: [
            () => {
                compare(late.count, 8, "the model set after creation is shown")
                compare(late.currentRow, -1, "setting a model selects nothing")
                compare(late.view.currentIndex, -1, "the view agrees")
                compare(preset.currentRow, 2, "a selection made by the caller is kept")
                compare(preset.view.currentIndex, 2, "the view shows the caller's selection")
                compare(itemModel.currentRow, -1, "an item model that fills up later selects nothing")
                late.model = rows(4, "again")
                preset.model = rows(9, "again")
            },
            () => {
                compare(late.currentRow, -1, "replacing the model selects nothing")
                compare(preset.currentRow, 2, "replacing the model keeps the caller's selection")
                compare(preset.view.currentIndex, 2, "…also in the view")
                late.currentRow = 3
            },
            () => {
                compare(late.view.currentIndex, 3, "currentRow set by the caller reaches the view")
                late.view.currentIndex = 1          // what the arrow keys do
            },
            () => {
                compare(late.currentRow, 1, "a selection made in the view (keyboard) reaches currentRow")
                late.currentRow = -1
            },
            () => {
                compare(late.view.currentIndex, -1, "selection cleared")
                late.model = null
                late.model = rows(3, "third")
            },
            () => compare(late.currentRow, -1, "model removed and set again: still nothing selected")
        ]
    }
}
