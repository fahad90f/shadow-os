// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Mouse & Touchpad")
    subtitle: qsTr("Pointer speed, buttons, scrolling and touchpad gestures of each device.")

    // Bumped when KWin refuses a change, so controls fall back to the real value.
    property int revision: 0

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the devices again")
            onClicked: mouse.refresh()
        }
    ]

    MouseBackend {
        id: mouse
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
    }

    // A boolean libinput setting of one device.
    component ToggleRow: NxRow {
        id: toggleRow
        property var backend
        property string sysName: ""
        property string setting: ""
        property bool current: false
        property int revision: 0

        Switch {
            anchors.verticalCenter: parent.verticalCenter
            checked: (toggleRow.revision, toggleRow.current)
            Accessible.name: toggleRow.title
            onToggled: toggleRow.backend.setBool(toggleRow.sysName, toggleRow.setting, checked)
        }
    }

    // A real-valued libinput setting, written when the slider is released.
    component SliderRow: NxRow {
        id: sliderRow
        property var backend
        property string sysName: ""
        property string setting: ""
        property real current: 0
        property real minimum: 0
        property real maximum: 1
        property real step: 0.1
        property string lowLabel: ""
        property string highLabel: ""
        property int revision: 0

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: sliderRow.lowLabel
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
        }
        Slider {
            id: slider
            anchors.verticalCenter: parent.verticalCenter
            width: 220
            from: sliderRow.minimum
            to: sliderRow.maximum
            stepSize: sliderRow.step
            snapMode: Slider.SnapAlways
            value: (sliderRow.revision, sliderRow.current)
            Accessible.name: sliderRow.title
            function commit() {
                if (Math.abs(value - sliderRow.current) > 0.0001)
                    sliderRow.backend.setReal(sliderRow.sysName, sliderRow.setting, value)
            }
            onPressedChanged: if (!pressed) commit()
            onMoved: if (!pressed) commit()
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: sliderRow.highLabel
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxCard {
        visible: mouse.ready && !mouse.available
        Layout.fillWidth: true
        implicitHeight: 300
        NxEmptyState {
            anchors.fill: parent
            icon: "mouse"
            tone: mouse.state === "no-devices" ? "neutral" : "warning"
            title: mouse.state === "no-devices" ? qsTr("No pointer device found")
                 : mouse.state === "no-input-api" ? qsTr("KWin does not expose its input devices")
                 : mouse.state === "no-session-bus" ? qsTr("No session bus")
                 : qsTr("KWin is not running")
            message: mouse.statusMessage
            actionText: qsTr("Try again")
            onActionTriggered: mouse.refresh()
        }
    }

    Repeater {
        model: mouse.devices

        NxSection {
            id: device
            required property string sysName
            required property string name
            required property bool touchpad
            required property var properties

            readonly property var scrollMethods: {
                const list = []
                if (properties.supportsScrollTwoFinger) list.push({ value: "scrollTwoFinger", text: qsTr("Two fingers") })
                if (properties.supportsScrollEdge) list.push({ value: "scrollEdge", text: qsTr("Touchpad edge") })
                if (properties.supportsScrollOnButtonDown) list.push({ value: "scrollOnButtonDown", text: qsTr("While a button is held") })
                return list
            }
            readonly property var clickMethods: {
                const list = []
                if (properties.supportsClickMethodAreas) list.push({ value: "clickMethodAreas", text: qsTr("Bottom corners of the pad") })
                if (properties.supportsClickMethodClickfinger) list.push({ value: "clickMethodClickfinger", text: qsTr("Number of fingers") })
                return list
            }
            function activeIndex(methods) {
                for (let i = 0; i < methods.length; i++) {
                    if (properties[methods[i].value] === true) return i
                }
                return -1
            }

            title: name
            description: touchpad ? qsTr("Touchpad  ·  %1").arg(sysName) : qsTr("Mouse  ·  %1").arg(sysName)

            NxRow {
                icon: device.touchpad ? "touchpad" : "mouse"
                title: qsTr("Use this device")
                subtitle: device.properties.supportsDisableEvents === true
                    ? "" : qsTr("This device cannot be switched off.")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: device.properties.supportsDisableEvents === true
                    checked: (page.revision, device.properties.enabled === true)
                    Accessible.name: qsTr("Use %1").arg(device.name)
                    onToggled: mouse.setBool(device.sysName, "enabled", checked)
                }
            }

            SliderRow {
                visible: device.properties.supportsPointerAcceleration === true
                icon: "gauge"
                title: qsTr("Pointer speed")
                backend: mouse
                sysName: device.sysName
                setting: "pointerAcceleration"
                current: Number(device.properties.pointerAcceleration || 0)
                minimum: -1
                maximum: 1
                step: 0.05
                lowLabel: qsTr("Slow")
                highLabel: qsTr("Fast")
                revision: page.revision
            }
            NxRow {
                visible: device.properties.supportsPointerAccelerationProfileFlat === true
                         && device.properties.supportsPointerAccelerationProfileAdaptive === true
                icon: "activity"
                title: qsTr("Pointer acceleration")
                subtitle: qsTr("Move the pointer further when the device moves faster")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (page.revision, device.properties.pointerAccelerationProfileAdaptive === true)
                    Accessible.name: qsTr("Pointer acceleration")
                    onToggled: mouse.setBool(device.sysName, checked ? "pointerAccelerationProfileAdaptive"
                                                                     : "pointerAccelerationProfileFlat", true)
                }
            }
            ToggleRow {
                visible: device.properties.supportsLeftHanded === true
                icon: "mouse"
                title: qsTr("Left-handed")
                subtitle: qsTr("Swap the left and right buttons")
                backend: mouse
                sysName: device.sysName
                setting: "leftHanded"
                current: device.properties.leftHanded === true
                revision: page.revision
            }
            ToggleRow {
                visible: device.properties.supportsMiddleEmulation === true
                icon: "target"
                title: qsTr("Middle click by pressing both buttons")
                backend: mouse
                sysName: device.sysName
                setting: "middleEmulation"
                current: device.properties.middleEmulation === true
                revision: page.revision
            }

            // ---- touchpad taps and clicks --------------------------------------
            ToggleRow {
                visible: Number(device.properties.tapFingerCount || 0) > 0
                icon: "touchpad"
                title: qsTr("Tap to click")
                backend: mouse
                sysName: device.sysName
                setting: "tapToClick"
                current: device.properties.tapToClick === true
                revision: page.revision
            }
            ToggleRow {
                visible: Number(device.properties.tapFingerCount || 0) > 0
                enabled: device.properties.tapToClick === true
                icon: "grip"
                title: qsTr("Tap and drag")
                subtitle: qsTr("Tap, then touch again and move to drag")
                backend: mouse
                sysName: device.sysName
                setting: "tapAndDrag"
                current: device.properties.tapAndDrag === true
                revision: page.revision
            }
            ToggleRow {
                visible: Number(device.properties.tapFingerCount || 0) > 0
                enabled: device.properties.tapToClick === true && device.properties.tapAndDrag === true
                icon: "lock"
                title: qsTr("Drag lock")
                subtitle: qsTr("Keep dragging when the finger is lifted briefly")
                backend: mouse
                sysName: device.sysName
                setting: "tapDragLock"
                current: device.properties.tapDragLock === true
                revision: page.revision
            }
            ToggleRow {
                visible: device.properties.supportsLmrTapButtonMap === true
                enabled: device.properties.tapToClick === true
                icon: "target"
                title: qsTr("Two-finger tap is a middle click")
                subtitle: qsTr("Otherwise two fingers are a right click and three a middle click")
                backend: mouse
                sysName: device.sysName
                setting: "lmrTapButtonMap"
                current: device.properties.lmrTapButtonMap === true
                revision: page.revision
            }
            NxRow {
                visible: device.clickMethods.length > 1
                icon: "mouse"
                title: qsTr("Right click by")
                ComboBox {
                    width: 240
                    model: device.clickMethods
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: (page.revision, device.activeIndex(device.clickMethods))
                    Accessible.name: qsTr("Right click by")
                    onActivated: mouse.setBool(device.sysName, currentValue, true)
                }
            }

            // ---- scrolling ---------------------------------------------------------
            NxRow {
                visible: device.scrollMethods.length > 1
                icon: "sort"
                title: qsTr("Scroll with")
                ComboBox {
                    width: 240
                    model: device.scrollMethods
                    textRole: "text"
                    valueRole: "value"
                    currentIndex: (page.revision, device.activeIndex(device.scrollMethods))
                    displayText: currentIndex < 0 ? qsTr("Scrolling is off") : currentText
                    Accessible.name: qsTr("Scroll with")
                    onActivated: mouse.setBool(device.sysName, currentValue, true)
                }
            }
            ToggleRow {
                visible: device.properties.supportsNaturalScroll === true
                icon: "arrow-down"
                title: qsTr("Natural scrolling")
                subtitle: qsTr("The content follows the movement of the fingers or the wheel")
                backend: mouse
                sysName: device.sysName
                setting: "naturalScroll"
                current: device.properties.naturalScroll === true
                revision: page.revision
            }
            SliderRow {
                visible: device.properties.scrollFactor !== undefined
                icon: "sliders"
                title: qsTr("Scrolling speed")
                backend: mouse
                sysName: device.sysName
                setting: "scrollFactor"
                current: Number(device.properties.scrollFactor || 1)
                minimum: 0.25
                maximum: 4
                step: 0.25
                lowLabel: qsTr("Slow")
                highLabel: qsTr("Fast")
                revision: page.revision
            }

            // ---- touchpad comfort ------------------------------------------------------
            ToggleRow {
                visible: device.properties.supportsDisableWhileTyping === true
                icon: "keyboard"
                title: qsTr("Ignore the touchpad while typing")
                backend: mouse
                sysName: device.sysName
                setting: "disableWhileTyping"
                current: device.properties.disableWhileTyping === true
                revision: page.revision
            }
            ToggleRow {
                visible: device.properties.supportsDisableEventsOnExternalMouse === true
                icon: "mouse"
                title: qsTr("Switch off while a mouse is connected")
                backend: mouse
                sysName: device.sysName
                setting: "disableEventsOnExternalMouse"
                current: device.properties.disableEventsOnExternalMouse === true
                revision: page.revision
            }
        }
    }
}
