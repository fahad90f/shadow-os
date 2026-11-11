// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQml.Models
import org.nexus.ui
import org.nexus.screenshot

NxWindow {
    id: window
    title: qsTr("Screenshot")
    width: 1120
    height: 780
    minimumWidth: 900
    minimumHeight: 560
    visible: shot.windowVisible

    readonly property var modeKeys: ["full", "monitor", "window", "pick", "region"]
    readonly property var modeLabels: [qsTr("Full screen"), qsTr("Monitor"), qsTr("Active window"), qsTr("Pick window"), qsTr("Region")]
    readonly property var toolKeys: ["crop", "arrow", "rect", "text", "blur"]
    readonly property var toolLabels: [qsTr("Crop"), qsTr("Arrow"), qsTr("Rectangle"), qsTr("Text"), qsTr("Pixelate")]
    readonly property var inkColors: [Theme.danger, Theme.warning, Theme.success, Theme.accentBlue, Theme.accentViolet]
    readonly property var inkNames: [qsTr("Red"), qsTr("Amber"), qsTr("Green"), qsTr("Blue"), qsTr("Violet")]
    property int inkIndex: 0
    readonly property var formatLabels: ({ "png": "PNG", "jpeg": "JPEG", "webp": "WebP" })

    onClosing: shot.windowClosed()
    onVisibleChanged: if (visible) { raise(); requestActivate() }

    Connections {
        target: shot
        function onMessage(text, kind) { window.toaster.show(text, kind) }
    }

    // One full-screen selection surface per monitor while a region is chosen.
    Instantiator {
        model: shot.selectingRegion ? shot.screens : []
        delegate: RegionOverlay {
            required property var modelData
            screenInfo: modelData
        }
    }

    Shortcut { sequences: [StandardKey.Undo]; enabled: shot.document.canUndo; onActivated: shot.document.undo() }
    Shortcut { sequences: [StandardKey.Redo]; enabled: shot.document.canRedo; onActivated: shot.document.redo() }
    Shortcut { sequences: [StandardKey.Save]; enabled: !shot.document.empty; onActivated: shot.save() }
    Shortcut { sequences: [StandardKey.Copy]; enabled: !shot.document.empty && !textPopup.opened; onActivated: shot.copyToClipboard() }
    Shortcut { sequence: "Escape"; enabled: view.tool !== "" && !textPopup.opened; onActivated: view.tool = "" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceLg
        spacing: Theme.spaceMd

        // ---- new capture -------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            NxSegmented {
                model: window.modeLabels
                currentIndex: Math.max(0, window.modeKeys.indexOf(shot.mode))
                onActivated: (index) => shot.mode = window.modeKeys[index]
            }
            ComboBox {
                id: monitorBox
                visible: shot.mode === "monitor"
                Layout.preferredWidth: 220
                model: shot.screens
                textRole: "label"
                valueRole: "name"
                Accessible.name: qsTr("Monitor to capture")
                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(shot.monitor))
                onActivated: shot.monitor = currentValue
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("Delay")
                color: Theme.textSecondary
            }
            SpinBox {
                id: delayBox
                from: 0
                to: 60
                value: shot.delay
                editable: true
                Layout.preferredWidth: 110
                Accessible.name: qsTr("Delay before the capture, in seconds")
                textFromValue: (v) => qsTr("%1 s").arg(v)
                valueFromText: (t) => parseInt(t) || 0
                onValueModified: shot.delay = value
            }
            Switch {
                text: qsTr("Pointer")
                checked: shot.includePointer
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Include the mouse pointer in the picture")
                onToggled: shot.includePointer = checked
            }
            NxButton {
                kind: "primary"
                iconName: "camera"
                text: qsTr("Capture")
                busy: shot.busy
                onClicked: shot.capture()
            }
        }

        NxBanner {
            visible: shot.error !== "" && !shot.document.empty
            kind: "danger"
            title: qsTr("The last capture failed")
            message: shot.error
        }

        // ---- picture -----------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusMd
            color: Theme.bgSurface
            border.width: Theme.borderWidth
            border.color: Theme.borderSubtle
            clip: true

            AnnotationView {
                id: view
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                visible: !shot.document.empty
                document: shot.document
                color: window.inkColors[window.inkIndex]
                frameColor: Theme.border
                Accessible.name: qsTr("Captured picture")
                onTextRequested: (imageX, imageY, itemX, itemY) => {
                    textPopup.imageX = imageX
                    textPopup.imageY = imageY
                    textPopup.x = Math.min(Math.max(0, itemX), view.width - textPopup.width)
                    textPopup.y = Math.min(Math.max(0, itemY), view.height - textPopup.height)
                    textField.text = ""
                    textPopup.open()
                }

                Popup {
                    id: textPopup
                    property real imageX: 0
                    property real imageY: 0
                    width: 320
                    padding: Theme.spaceSm
                    focus: true
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                    onOpened: textField.forceActiveFocus()
                    contentItem: RowLayout {
                        spacing: Theme.spaceSm
                        TextField {
                            id: textField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Text to place here")
                            Accessible.name: qsTr("Annotation text")
                            onAccepted: addText.clicked()
                        }
                        NxButton {
                            id: addText
                            kind: "primary"
                            text: qsTr("Add")
                            enabled: textField.text.trim() !== ""
                            onClicked: { view.placeText(textPopup.imageX, textPopup.imageY, textField.text); textPopup.close() }
                        }
                    }
                }
            }

            NxEmptyState {
                anchors.fill: parent
                visible: shot.document.empty && shot.error !== ""
                icon: "screenshot"
                tone: "danger"
                title: qsTr("The screen could not be captured")
                message: shot.error
                actionText: qsTr("Try again")
                onActionTriggered: shot.capture()
            }
            NxEmptyState {
                anchors.fill: parent
                visible: shot.document.empty && shot.error === ""
                icon: "screenshot"
                title: shot.busy ? qsTr("Capturing…") : qsTr("No picture yet")
                message: shot.busy ? "" : qsTr("Choose what to capture above, then press Capture.")
            }
        }

        // ---- annotate ----------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            enabled: !shot.document.empty

            Repeater {
                model: window.toolKeys
                Button {
                    required property string modelData
                    required property int index
                    text: window.toolLabels[index]
                    checkable: true
                    checked: view.tool === modelData
                    onClicked: view.tool = (view.tool === modelData ? "" : modelData)
                }
            }
            NxDivider { vertical: true; Layout.preferredHeight: Theme.controlHeight; Layout.leftMargin: Theme.spaceXs; Layout.rightMargin: Theme.spaceXs }
            Repeater {
                model: window.inkColors
                Rectangle {
                    id: swatch
                    required property color modelData
                    required property int index
                    readonly property bool selected: window.inkIndex === index
                    implicitWidth: Theme.controlHeight - Theme.spaceSm
                    implicitHeight: implicitWidth
                    radius: width / 2
                    color: modelData
                    border.width: selected ? 3 : Theme.borderWidth
                    border.color: selected ? Theme.textPrimary : Theme.border
                    activeFocusOnTab: true
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: qsTr("Annotation colour: %1").arg(window.inkNames[index])
                    Accessible.checked: selected
                    Keys.onSpacePressed: window.inkIndex = index
                    Keys.onReturnPressed: window.inkIndex = index
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        radius: width / 2
                        color: "transparent"
                        border.width: Theme.focusRing
                        border.color: Theme.accent
                        visible: swatch.activeFocus
                    }
                    TapHandler { onTapped: window.inkIndex = swatch.index }
                }
            }
            NxDivider { vertical: true; Layout.preferredHeight: Theme.controlHeight; Layout.leftMargin: Theme.spaceXs; Layout.rightMargin: Theme.spaceXs }
            NxIconButton {
                iconName: "undo"
                tooltip: qsTr("Undo")
                enabled: shot.document.canUndo
                onClicked: shot.document.undo()
            }
            NxIconButton {
                iconName: "redo"
                tooltip: qsTr("Redo")
                enabled: shot.document.canRedo
                onClicked: shot.document.redo()
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: !shot.document.empty
                text: qsTr("%1 × %2 px · shown at %3 %").arg(shot.document.width).arg(shot.document.height).arg(Math.round(view.zoom * 100))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
            }
        }

        // ---- keep --------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                text: shot.savedPath !== "" ? qsTr("Saved: %1").arg(shot.savedPath)
                    : shot.document.empty ? "" : qsTr("Not saved — Save writes to %1").arg(shot.saveDirectory)
            }
            ComboBox {
                id: formatBox
                Layout.preferredWidth: 110
                model: shot.formats.map(f => ({ "value": f, "text": window.formatLabels[f] }))
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("File format")
                Component.onCompleted: currentIndex = Math.max(0, indexOfValue(shot.format))
                onActivated: shot.format = currentValue
            }
            NxButton {
                text: qsTr("Open folder")
                iconName: "folder-open"
                onClicked: shot.openFolder()
            }
            NxButton {
                text: qsTr("Save as…")
                enabled: !shot.document.empty
                onClicked: { pathField.text = shot.suggestedPath(); saveAsDialog.open() }
            }
            NxButton {
                text: qsTr("Copy")
                iconName: "copy"
                enabled: !shot.document.empty
                onClicked: shot.copyToClipboard()
            }
            NxButton {
                kind: "primary"
                text: qsTr("Save")
                iconName: "save"
                enabled: !shot.document.empty
                onClicked: shot.save()
            }
        }
    }

    Dialog {
        id: saveAsDialog
        title: qsTr("Save screenshot as")
        anchors.centerIn: parent
        width: Math.min(620, window.width - Theme.spaceXxl)
        modal: true
        footer: null
        onOpened: { pathField.forceActiveFocus(); pathField.selectAll() }
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("File name with folder. The ending decides the format: .png, .jpg or .webp.")
            }
            TextField {
                id: pathField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("File to save to")
                onAccepted: if (saveAsConfirm.enabled) saveAsConfirm.clicked()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: saveAsDialog.close() }
                NxButton {
                    id: saveAsConfirm
                    kind: "primary"
                    text: qsTr("Save")
                    enabled: pathField.text.trim() !== ""
                    onClicked: if (shot.saveAs(pathField.text)) saveAsDialog.close()
                }
            }
        }
    }
}
