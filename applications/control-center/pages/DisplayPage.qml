// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Displays")
    subtitle: qsTr("Resolution, scaling, orientation and arrangement of the connected displays.")

    // Bumped to make controls fall back to the back-end's value (refused
    // changes, dropped outputs).
    property int revision: 0

    readonly property var orientations: [
        { value: "none", text: qsTr("Landscape") },
        { value: "right", text: qsTr("Portrait — rotated 90° clockwise") },
        { value: "inverted", text: qsTr("Landscape — upside down") },
        { value: "left", text: qsTr("Portrait — rotated 90° counter-clockwise") },
    ]
    readonly property var vrrPolicies: [
        { value: "never", text: qsTr("Never") },
        { value: "automatic", text: qsTr("Automatic") },
        { value: "always", text: qsTr("Always") },
    ]
    readonly property var nightModes: [
        { value: "times", text: qsTr("Custom times") },
        { value: "location", text: qsTr("Sunset to sunrise") },
        { value: "constant", text: qsTr("Always on") },
    ]

    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) {
            if (items[i].value === value) return i
        }
        return -1
    }

    actions: [
        NxButton {
            visible: screens.available && screens.dirty
            text: qsTr("Discard")
            kind: "ghost"
            enabled: !screens.busy
            onClicked: { screens.discard(); page.revision++ }
        },
        NxButton {
            visible: screens.available
            text: qsTr("Apply")
            kind: "primary"
            iconName: "check"
            enabled: screens.dirty && !screens.confirming
            busy: screens.busy
            onClicked: screens.apply()
        }
    ]

    DisplayBackend {
        id: screens
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onReverted: (automatically) => page.ApplicationWindow.window.toaster.show(
            automatically ? qsTr("The previous display settings were restored because the new ones were not confirmed.")
                          : qsTr("The previous display settings were restored."), "info")
        onConfirmingChanged: {
            if (screens.confirming) confirmDialog.open()
            else confirmDialog.close()
        }
    }

    NxCard {
        visible: screens.ready && !screens.available
        Layout.fillWidth: true
        implicitHeight: 320
        NxEmptyState {
            anchors.fill: parent
            icon: "monitor"
            tone: "warning"
            title: screens.state === "no-tool" ? qsTr("kscreen-doctor is not installed")
                 : screens.state === "query-failed" ? qsTr("The display configuration could not be read")
                 : screens.state === "no-outputs" ? qsTr("No display is connected")
                 : qsTr("Not running under KWin Wayland")
            message: screens.statusMessage
            actionText: qsTr("Try again")
            onActionTriggered: screens.refresh()
        }
    }

    NxBanner {
        visible: screens.available && screens.dirty && !screens.confirming
        kind: "info"
        title: qsTr("Changes are not applied yet")
        message: qsTr("Apply them to see the result. You will be asked to confirm; without confirmation the previous settings come back after 15 seconds.")
        actionText: qsTr("Apply")
        onActionTriggered: screens.apply()
    }

    // ---- arrangement ------------------------------------------------------------
    NxSection {
        visible: screens.available && screens.enabledCount > 1
        title: qsTr("Arrangement")
        description: qsTr("Drag a display to where it stands on your desk, or select it and use the arrow keys. Displays snap to each other.")

        Item {
            id: canvas
            Layout.fillWidth: true
            implicitHeight: 230

            readonly property rect bounds: screens.layoutBounds
            readonly property real margin: Theme.spaceXl
            readonly property real factor: bounds.width > 0 && bounds.height > 0
                ? Math.min((width - 2 * margin) / bounds.width, (height - 2 * margin) / bounds.height) : 1
            readonly property real originX: (width - bounds.width * factor) / 2 - bounds.x * factor
            readonly property real originY: (height - bounds.height * factor) / 2 - bounds.y * factor

            Repeater {
                model: screens.outputs

                Rectangle {
                    id: box
                    required property string name
                    required property bool outputEnabled
                    required property int posX
                    required property int posY
                    required property int logicalWidth
                    required property int logicalHeight
                    required property bool primary
                    required property string modeText

                    function drop() {
                        screens.moveOutput(name, Math.round((x - canvas.originX) / canvas.factor),
                                           Math.round((y - canvas.originY) / canvas.factor))
                        page.revision++
                    }

                    visible: outputEnabled
                    x: (page.revision, canvas.originX + posX * canvas.factor)
                    y: (page.revision, canvas.originY + posY * canvas.factor)
                    z: drag.active ? 2 : 1
                    width: logicalWidth * canvas.factor
                    height: logicalHeight * canvas.factor
                    radius: Theme.radiusSm
                    color: drag.active ? Theme.accentFillStrong : hover.hovered ? Theme.accentFill : Theme.bgHover
                    border.width: activeFocus ? Theme.focusRing : Theme.borderWidth
                    border.color: activeFocus || primary ? Theme.accent : Theme.borderStrong

                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: qsTr("Display %1").arg(name)
                    Accessible.description: qsTr("Use the arrow keys to move it to another side of its neighbour")
                    Keys.onLeftPressed: { screens.moveOutput(name, posX - logicalWidth, posY); page.revision++ }
                    Keys.onRightPressed: { screens.moveOutput(name, posX + logicalWidth, posY); page.revision++ }
                    Keys.onUpPressed: { screens.moveOutput(name, posX, posY - logicalHeight); page.revision++ }
                    Keys.onDownPressed: { screens.moveOutput(name, posX, posY + logicalHeight); page.revision++ }

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - Theme.spaceSm * 2
                        spacing: 0
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: box.name
                            font.weight: Theme.weightSemibold
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            visible: box.height > 56
                            text: box.primary ? qsTr("Primary") + "  ·  " + box.modeText : box.modeText
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeCaption
                            elide: Text.ElideRight
                        }
                    }

                    HoverHandler { id: hover; cursorShape: Qt.SizeAllCursor }
                    TapHandler { onTapped: box.forceActiveFocus() }
                    DragHandler {
                        id: drag
                        onActiveChanged: if (!active) box.drop()
                    }
                }
            }
        }
    }

    // ---- one section per display -----------------------------------------------------
    Repeater {
        model: screens.outputs

        NxSection {
            id: out
            required property string name
            required property string outputTitle
            required property bool outputEnabled
            required property string modeText
            required property var resolutions
            required property int resolutionIndex
            required property var refreshRates
            required property int refreshIndex
            required property var scales
            required property int scaleIndex
            required property string orientation
            required property bool primary
            required property bool vrrCapable
            required property string vrrPolicy
            required property bool hdrCapable
            required property bool hdr
            required property string physicalSize
            required property bool changed

            readonly property bool lastActive: outputEnabled && screens.enabledCount <= 1

            title: outputTitle
            description: {
                const parts = []
                if (!outputEnabled) parts.push(qsTr("Switched off"))
                else if (modeText !== "") parts.push(modeText)
                if (physicalSize !== "") parts.push(physicalSize)
                if (changed) parts.push(qsTr("changed, not applied yet"))
                return parts.join("  ·  ")
            }

            NxRow {
                icon: "monitor"
                title: qsTr("Use this display")
                subtitle: out.lastActive ? qsTr("The only active display cannot be switched off.") : ""
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (page.revision, out.outputEnabled)
                    enabled: !out.lastActive
                    Accessible.name: qsTr("Use display %1").arg(out.name)
                    onToggled: screens.setOutputEnabled(out.name, checked)
                }
            }
            NxRow {
                visible: out.outputEnabled
                icon: "maximize"
                title: qsTr("Resolution")
                ComboBox {
                    width: 200
                    model: out.resolutions
                    textRole: "text"
                    currentIndex: (page.revision, out.resolutionIndex)
                    Accessible.name: qsTr("Resolution of %1").arg(out.name)
                    onActivated: (index) => screens.setResolution(out.name, out.resolutions[index].width, out.resolutions[index].height)
                }
            }
            NxRow {
                visible: out.outputEnabled
                icon: "activity"
                title: qsTr("Refresh rate")
                ComboBox {
                    width: 200
                    model: out.refreshRates
                    textRole: "text"
                    enabled: out.refreshRates.length > 1
                    currentIndex: (page.revision, out.refreshIndex)
                    Accessible.name: qsTr("Refresh rate of %1").arg(out.name)
                    onActivated: (index) => screens.setMode(out.name, out.refreshRates[index].modeId)
                }
            }
            NxRow {
                visible: out.outputEnabled
                icon: "zoom-in"
                title: qsTr("Scale")
                subtitle: qsTr("Size of text and interface elements")
                ComboBox {
                    width: 200
                    model: out.scales
                    textRole: "text"
                    currentIndex: (page.revision, out.scaleIndex)
                    Accessible.name: qsTr("Scale of %1").arg(out.name)
                    onActivated: (index) => screens.setScale(out.name, out.scales[index].value)
                }
            }
            NxRow {
                visible: out.outputEnabled
                icon: "refresh"
                title: qsTr("Orientation")
                ComboBox {
                    width: 300
                    model: page.orientations
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: (page.revision, page.indexOfValue(page.orientations, out.orientation))
                    Accessible.name: qsTr("Orientation of %1").arg(out.name)
                    onActivated: screens.setRotation(out.name, currentValue)
                }
            }
            NxRow {
                visible: out.outputEnabled && screens.enabledCount > 1
                icon: "star"
                title: qsTr("Primary display")
                subtitle: qsTr("The panel and new windows appear here first")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (page.revision, out.primary)
                    enabled: !out.primary
                    Accessible.name: qsTr("Make %1 the primary display").arg(out.name)
                    onToggled: screens.setPrimary(out.name)
                }
            }
            NxRow {
                visible: out.outputEnabled && out.vrrCapable
                icon: "zap"
                title: qsTr("Variable refresh rate")
                subtitle: qsTr("Adaptive sync (FreeSync, G-Sync compatible)")
                ComboBox {
                    width: 200
                    model: page.vrrPolicies
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: (page.revision, page.indexOfValue(page.vrrPolicies, out.vrrPolicy))
                    Accessible.name: qsTr("Variable refresh rate of %1").arg(out.name)
                    onActivated: screens.setVrrPolicy(out.name, currentValue)
                }
            }
            NxRow {
                visible: out.outputEnabled && out.hdrCapable
                icon: "sun"
                title: qsTr("HDR")
                subtitle: qsTr("High dynamic range and wide colour gamut")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (page.revision, out.hdr)
                    Accessible.name: qsTr("HDR on %1").arg(out.name)
                    onToggled: screens.setHdr(out.name, checked)
                }
            }
        }
    }

    // ---- Night Light -------------------------------------------------------------------
    NxSection {
        visible: screens.available && screens.nightLightAvailable
        title: qsTr("Night Light")
        description: qsTr("Makes the colours warmer in the evening and at night.")

        NxRow {
            icon: "night-light"
            title: qsTr("Night Light")
            subtitle: !screens.nightLightSupported ? qsTr("The graphics driver of this session cannot change the colour temperature.")
                    : screens.nightLightRunning ? qsTr("On — the screen is at %1 K right now").arg(screens.nightLightCurrentTemperature)
                    : screens.nightLightEnabled ? qsTr("On — waiting for the scheduled time") : ""
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: (page.revision, screens.nightLightEnabled)
                enabled: screens.nightLightSupported
                Accessible.name: qsTr("Night Light")
                onToggled: screens.setNightLightEnabled(checked)
            }
        }
        NxRow {
            icon: "thermometer"
            title: qsTr("Colour temperature at night")
            subtitle: qsTr("Lower values are warmer")
            enabled: screens.nightLightSupported
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%1 K").arg(Math.round(temperature.value))
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: temperature
                anchors.verticalCenter: parent.verticalCenter
                width: 240
                from: 1000
                to: 6500
                stepSize: 100
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Colour temperature at night in kelvin")
                onMoved: {
                    if (pressed) screens.previewNightTemperature(Math.round(value))
                    else screens.setNightTemperature(Math.round(value))
                }
                onPressedChanged: if (!pressed) screens.setNightTemperature(Math.round(value))
                Binding {
                    target: temperature
                    property: "value"
                    value: screens.nightTemperature
                    when: !temperature.pressed
                    restoreMode: Binding.RestoreNone
                }
            }
        }
        NxRow {
            icon: "clock"
            title: qsTr("Schedule")
            subtitle: screens.nightLightMode === "automatic"
                ? qsTr("Set to follow an automatically detected location, which needs a location service that NEXUS does not run. Choose a schedule.")
                : ""
            enabled: screens.nightLightSupported
            ComboBox {
                width: 220
                model: page.nightModes
                textRole: "text"
                valueRole: "value"
                currentIndex: (page.revision, page.indexOfValue(page.nightModes, screens.nightLightMode))
                displayText: currentIndex < 0 ? qsTr("Automatic location") : currentText
                Accessible.name: qsTr("Night Light schedule")
                onActivated: screens.setNightLightMode(currentValue)
            }
        }
        NxRow {
            visible: screens.nightLightMode === "times"
            icon: "moon"
            title: qsTr("Turn on at")
            enabled: screens.nightLightSupported
            TextField {
                id: eveningField
                width: 90
                horizontalAlignment: TextInput.AlignHCenter
                font.family: Theme.fontMono
                text: (page.revision, screens.nightLightEvening)
                inputMethodHints: Qt.ImhTime
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                Accessible.name: qsTr("Night Light turns on at")
                onEditingFinished: if (text !== screens.nightLightEvening) screens.setNightLightTimes(text, screens.nightLightMorning)
            }
        }
        NxRow {
            visible: screens.nightLightMode === "times"
            icon: "sun"
            title: qsTr("Turn off at")
            enabled: screens.nightLightSupported
            TextField {
                id: morningField
                width: 90
                horizontalAlignment: TextInput.AlignHCenter
                font.family: Theme.fontMono
                text: (page.revision, screens.nightLightMorning)
                inputMethodHints: Qt.ImhTime
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                Accessible.name: qsTr("Night Light turns off at")
                onEditingFinished: if (text !== screens.nightLightMorning) screens.setNightLightTimes(screens.nightLightEvening, text)
            }
        }
        NxRow {
            visible: screens.nightLightMode === "times"
            icon: "timer"
            title: qsTr("Transition")
            subtitle: qsTr("Minutes the colours take to change")
            enabled: screens.nightLightSupported
            SpinBox {
                from: 1
                to: 600
                editable: true
                value: (page.revision, screens.nightLightTransitionMinutes)
                Accessible.name: qsTr("Transition time in minutes")
                onValueModified: screens.setNightLightTransitionMinutes(value)
            }
        }
        NxRow {
            visible: screens.nightLightMode === "location"
            icon: "globe"
            title: qsTr("Location")
            subtitle: qsTr("Latitude and longitude in degrees; used to calculate sunset and sunrise")
            enabled: screens.nightLightSupported
            TextField {
                id: latitudeField
                width: 96
                font.family: Theme.fontMono
                placeholderText: qsTr("Latitude")
                text: (page.revision, screens.nightLightLatitude.toFixed(2))
                validator: DoubleValidator { bottom: -90; top: 90; decimals: 4; notation: DoubleValidator.StandardNotation; locale: "C" }
                Accessible.name: qsTr("Latitude")
                onEditingFinished: screens.setNightLightLocation(Number(text), Number(longitudeField.text))
            }
            TextField {
                id: longitudeField
                width: 96
                font.family: Theme.fontMono
                placeholderText: qsTr("Longitude")
                text: (page.revision, screens.nightLightLongitude.toFixed(2))
                validator: DoubleValidator { bottom: -180; top: 180; decimals: 4; notation: DoubleValidator.StandardNotation; locale: "C" }
                Accessible.name: qsTr("Longitude")
                onEditingFinished: screens.setNightLightLocation(Number(latitudeField.text), Number(text))
            }
        }
    }

    Dialog {
        id: confirmDialog
        title: qsTr("Keep these display settings?")
        width: Math.min(440, page.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        // Escape (or anything else that closes the dialog) means "no".
        onClosed: if (screens.confirming) screens.revert()

        contentItem: ColumnLayout {
            spacing: Theme.spaceLg
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: screens.confirmSecondsLeft === 1
                    ? qsTr("The previous settings are restored in 1 second.")
                    : qsTr("The previous settings are restored in %1 seconds.").arg(screens.confirmSecondsLeft)
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: 15
                value: screens.confirmSecondsLeft
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Revert")
                    onClicked: screens.revert()
                }
                NxButton {
                    text: qsTr("Keep")
                    kind: "primary"
                    onClicked: screens.keep()
                }
            }
        }
    }
}
