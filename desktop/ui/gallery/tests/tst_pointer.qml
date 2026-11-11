// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtTest
import org.shadow.ui

// Pointer tests of ShadowUI with real (synthesised) mouse events.
// Run inside the build chroot — see run-tests.sh in this directory.
Item {
    id: root
    width: 900
    height: 640

    property var log: []
    function note(what) { log.push(what) }
    function taken() { const s = log.join(","); log = []; return s }

    // ---- content underneath: pointer handlers and classic items ---------------
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase
        TapHandler { onTapped: root.note("under-tap") }
        TapHandler { acceptedButtons: Qt.RightButton; onTapped: root.note("under-right-tap") }
    }
    Column {
        anchors.fill: parent
        Repeater {
            model: 10
            NxRow {
                required property int index
                width: root.width
                title: "Row " + index
                clickable: true
                onClicked: root.note("under-row")
            }
        }
    }

    property int mode: 0
    Dialog {
        id: dialog
        x: 200; y: 40
        width: 500; height: 560
        title: "Dialog"
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.note("accepted")
        onRejected: root.note("rejected")
        ColumnLayout {
            anchors.fill: parent
            NxSegmented { id: segments; model: ["A", "B", "C"]; onActivated: (i) => root.note("segment " + i) }
            NxSegmented { id: boundSegments; model: ["X", "Y", "Z"]; currentIndex: root.mode; onActivated: (i) => root.mode = i }
            NxRow { id: innerRow; Layout.fillWidth: true; title: "Clickable row"; clickable: true; onClicked: root.note("inner-row") }
            Button { id: innerButton; text: "Button"; onClicked: root.note("inner-button") }
            Slider { id: innerSlider; Layout.fillWidth: true; from: 0; to: 100; value: 10 }
            NxDataTable {
                id: innerTable
                Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 150
                columns: [{ title: "Name", role: "name", fill: true }]
                model: [{ name: "one" }, { name: "two" }, { name: "three" }]
                onRowSelected: (row) => root.note("table-row " + row)
                onContextMenuRequested: (row) => root.note("table-context " + row)
            }
        }
    }
    Popup {
        id: popup            // not modal
        x: 250; y: 150
        width: 300; height: 200
        closePolicy: Popup.NoAutoClose
        NxSegmented { id: popupSegments; model: ["P", "Q"]; onActivated: (i) => root.note("popup-segment " + i) }
    }
    Dialog {
        id: plain            // an application dialog that replaces the style's background
        x: 250; y: 150
        width: 300; height: 200
        background: Rectangle { color: "#202838" }
        NxSegmented { id: plainSegments; model: ["M", "N"]; onActivated: (i) => root.note("plain-segment " + i) }
    }
    NxToaster { id: toaster; anchors.fill: parent }

    function centre(item) { return item.mapToItem(root, item.width / 2, item.height / 2) }
    function clickItem(item, button) { const p = centre(item); mouseClickAt(p.x, p.y, button) }
    function mouseClickAt(x, y, button) { tc.mouseClick(root, x, y, button === undefined ? Qt.LeftButton : button); tc.wait(30) }
    function segment(seg, i) { return seg.children[0].children[i] }

    TestCase {
        id: tc
        name: "ShadowUIPointer"
        when: windowShown

        function init() { root.log = []; root.mode = 0 }
        function cleanup() {
            dialog.close(); popup.close(); plain.close()
            tryCompare(dialog, "visible", false); tryCompare(popup, "visible", false); tryCompare(plain, "visible", false)
        }
        function openAndWait(p) { p.open(); tryCompare(p, "opened", true); wait(50) }

        function test_underneath_reacts_without_popup() {
            mouseClickAt(450, 300)
            verify(root.taken().indexOf("under-") >= 0, "sanity: the content underneath reacts to a tap")
        }

        function test_modal_dialog_segment_does_not_fall_through() {
            openAndWait(dialog)
            clickItem(segment(segments, 1))
            compare(root.taken(), "segment 1")
            compare(segments.currentIndex, 1)
            clickItem(segment(segments, 2))
            compare(root.taken(), "segment 2")
        }

        function test_modal_dialog_bound_segment_keeps_binding() {
            openAndWait(dialog)
            clickItem(segment(boundSegments, 2))
            compare(root.mode, 2)
            compare(boundSegments.currentIndex, 2)
            compare(root.taken(), "")
            root.mode = 1
            compare(boundSegments.currentIndex, 1, "still bound after a tap")
        }

        function test_modal_dialog_other_content() {
            openAndWait(dialog)
            clickItem(innerRow);    compare(root.taken(), "inner-row")
            clickItem(innerButton); compare(root.taken(), "inner-button")
            const first = innerTable.view.itemAtIndex(1)
            verify(first !== null)
            clickItem(first);                 compare(root.taken(), "table-row 1")
            clickItem(first, Qt.RightButton); compare(root.taken(), "table-context 1")
            // empty area of the dialog and its title
            mouseClickAt(dialog.x + 250, dialog.y + 20); compare(root.taken(), "")
            mouseClickAt(dialog.x + 250, dialog.y + 20, Qt.RightButton); compare(root.taken(), "")
            verify(dialog.opened)
        }

        function test_modal_dialog_controls_still_work() {
            openAndWait(dialog)
            // dragging a Slider inside the dialog
            const p = centre(innerSlider)
            mousePress(root, innerSlider.mapToItem(root, 10, 0).x, p.y)
            mouseMove(root, p.x, p.y, 20)
            mouseMove(root, p.x + 100, p.y, 20)
            mouseRelease(root, p.x + 100, p.y)
            verify(innerSlider.value > 50, "slider dragged to " + innerSlider.value)
            compare(root.taken(), "")
            // standard button
            clickItem(dialog.standardButton(Dialog.Ok))
            compare(root.taken(), "accepted")
            tryCompare(dialog, "visible", false)
        }

        function test_modal_dialog_press_outside_closes_without_fall_through() {
            openAndWait(dialog)
            mouseClickAt(40, 40)                      // on the dimmed area
            tryCompare(dialog, "visible", false)
            compare(root.taken(), "rejected")
            mouseClickAt(40, 40)                      // now the content underneath is reachable again
            verify(root.taken().indexOf("under-") >= 0)
        }

        function test_modal_dialog_with_application_background() {
            openAndWait(plain)
            clickItem(segment(plainSegments, 1))
            compare(root.taken(), "plain-segment 1")
            mouseClickAt(plain.x + 150, plain.y + 150); compare(root.taken(), "")
        }

        function test_non_modal_popup() {
            openAndWait(popup)
            clickItem(segment(popupSegments, 1))
            compare(root.taken(), "popup-segment 1")
            mouseClickAt(popup.x + 150, popup.y + 150)       // empty area of the popup
            compare(root.taken(), "")
            mouseClickAt(40, 40)                             // outside a non-modal popup: reaches the content
            verify(root.taken().indexOf("under-") >= 0)
        }

        function test_toast_dismiss_does_not_fall_through() {
            toaster.show("A toast over the rows", "info")
            wait(400)
            const column = toaster.children[0]
            let toast = null
            for (let i = 0; i < column.children.length; i++)
                if (column.children[i].text !== undefined) toast = column.children[i]
            verify(toast !== null)
            clickItem(toast)
            compare(root.taken(), "")
            tryVerify(() => { for (let i = 0; i < column.children.length; i++) if (column.children[i].text !== undefined) return false; return true })
        }
    }
}
