// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Accessibility")
    subtitle: qsTr("Make the desktop easier to see and to use.")

    // Bumped when a change is refused, so controls return to the real state.
    property int revision: 0

    // Whether the UI library follows the stored text size is observed, not
    // assumed (see README-system.md).
    property real scaleAtLoad: 1
    property int bodySizeAtLoad: 0
    readonly property bool fontScaleIgnored: Math.abs(Appearance.fontScale - scaleAtLoad) > 0.01
                                             && Theme.fontSizeBody === bodySizeAtLoad
    Component.onCompleted: { scaleAtLoad = Appearance.fontScale; bodySizeAtLoad = Theme.fontSizeBody }

    readonly property var colorModes: [qsTr("Red–green (protanopia)"), qsTr("Red–green (deuteranopia)"), qsTr("Blue–yellow (tritanopia)")]

    AccessibilityBackend {
        id: a11y
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
    }

    // ---- seeing ---------------------------------------------------------------------
    NxSection {
        title: qsTr("Seeing")

        NxRow {
            icon: "type"
            title: qsTr("Text size")
            subtitle: page.fontScaleIgnored
                ? qsTr("Saved, but this version of the NEXUS UI library does not scale its text yet.")
                : qsTr("Scales the text of NEXUS applications.")
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: Math.round(fontSlider.value * 100) + " %"
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: fontSlider
                anchors.verticalCenter: parent.verticalCenter
                width: 220
                from: 0.8
                to: 1.6
                stepSize: 0.05
                snapMode: Slider.SnapAlways
                Accessible.name: qsTr("Text size")
                // Applied on release: re-laying out the page under the pointer
                // while dragging would move the slider away from it.
                onPressedChanged: if (!pressed) Appearance.fontScale = Math.round(value * 100) / 100
                Keys.onReleased: Appearance.fontScale = Math.round(value * 100) / 100
                Binding {
                    target: fontSlider
                    property: "value"
                    value: Appearance.fontScale
                    when: !fontSlider.pressed
                    restoreMode: Binding.RestoreNone
                }
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Reset")
                enabled: Math.abs(Appearance.fontScale - 1) > 0.001
                onClicked: Appearance.fontScale = 1
            }
        }
        NxRow {
            icon: "activity"
            title: qsTr("Reduce motion")
            subtitle: qsTr("Switches off animations in NEXUS applications and the shell.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: Appearance.reduceMotion
                Accessible.name: qsTr("Reduce motion")
                onToggled: Appearance.reduceMotion = checked
            }
        }
        NxRow {
            icon: "mouse"
            title: qsTr("Pointer size")
            subtitle: {
                const parts = [a11y.canPersist
                    ? qsTr("The compositor follows at once; applications that are already running may keep the old size until they are restarted.")
                    : qsTr("Not available: this build cannot write the compositor's input settings.")]
                if (a11y.sessionCursorSize !== "" && Number(a11y.sessionCursorSize) !== a11y.cursorSize)
                    parts.push(qsTr("This session was started with XCURSOR_SIZE=%1; X11 applications use the new size after the next login.").arg(a11y.sessionCursorSize))
                return parts.join(" ")
            }
            ComboBox {
                anchors.verticalCenter: parent.verticalCenter
                width: 150
                enabled: a11y.canPersist
                model: a11y.cursorSizes
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Pointer size")
                currentIndex: {
                    page.revision
                    for (let i = 0; i < a11y.cursorSizes.length; i++)
                        if (a11y.cursorSizes[i].value === a11y.cursorSize) return i
                    return -1
                }
                onActivated: a11y.setCursorSize(currentValue)
            }
        }
    }

    // ---- compositor effects -------------------------------------------------------------
    NxBanner {
        visible: a11y.ready && !a11y.compositorRunning
        kind: "warning"
        title: qsTr("Screen effects are not available")
        message: a11y.statusMessage
    }

    NxBanner {
        visible: a11y.compositorRunning && a11y.softwareRendering
        kind: "info"
        title: qsTr("Screen effects need hardware graphics")
        message: qsTr("The compositor is drawing with its software renderer (no OpenGL), with which the magnifier and the other KWin effects cannot draw. They can be switched on when the session runs with a graphics driver.")
    }

    NxSection {
        visible: a11y.compositorRunning && a11y.effects.length > 0
        title: qsTr("Screen effects")
        description: a11y.canPersist
            ? qsTr("Provided by the KWin compositor. They take effect at once and are remembered for your next login.")
            : qsTr("Provided by the KWin compositor. This build cannot save them, so they last until you log out.")

        Repeater {
            model: a11y.effects
            ColumnLayout {
                id: effect
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 0

                Rectangle {
                    visible: effect.index > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spaceLg
                    implicitHeight: 1
                    color: Theme.borderSubtle
                }
                NxRow {
                    icon: effect.modelData.icon
                    title: effect.modelData.title
                    subtitle: effect.modelData.supported || a11y.softwareRendering ? effect.modelData.description
                        : qsTr("%1 Not available: the compositor reports that it cannot run this effect on this hardware.").arg(effect.modelData.description)
                    Switch {
                        anchors.verticalCenter: parent.verticalCenter
                        enabled: effect.modelData.supported || effect.modelData.loaded
                        checked: (page.revision, effect.modelData.loaded)
                        Accessible.name: effect.modelData.title
                        onToggled: {
                            a11y.setEffectEnabled(effect.modelData.id, checked)
                            checked = Qt.binding(() => (page.revision, effect.modelData.loaded))
                        }
                    }
                }
                // Shortcuts of the effect, with buttons that do the same.
                Flow {
                    visible: effect.modelData.loaded && effect.modelData.supported && effect.modelData.actions.length > 0
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spaceLg + Theme.icon + Theme.spaceMd
                    Layout.rightMargin: Theme.spaceLg
                    Layout.bottomMargin: Theme.spaceMd
                    spacing: Theme.spaceSm

                    Repeater {
                        model: effect.modelData.actions
                        NxButton {
                            required property var modelData
                            text: modelData.keys !== "" ? qsTr("%1  (%2)").arg(modelData.label).arg(modelData.keys) : modelData.label
                            Accessible.name: modelData.label
                            onClicked: a11y.trigger(modelData.name)
                        }
                    }
                }
                RowLayout {
                    visible: effect.modelData.id === "colorblindnesscorrection" && effect.modelData.loaded && effect.modelData.supported
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spaceLg + Theme.icon + Theme.spaceMd
                    Layout.rightMargin: Theme.spaceLg
                    Layout.bottomMargin: Theme.spaceMd
                    spacing: Theme.spaceSm
                    Label { text: qsTr("Correct for"); color: Theme.textSecondary }
                    ComboBox {
                        Layout.preferredWidth: 260
                        enabled: a11y.canPersist
                        model: page.colorModes
                        currentIndex: (page.revision, a11y.colorblindMode)
                        Accessible.name: qsTr("Kind of colour blindness")
                        onActivated: (index) => a11y.setColorblindMode(index)
                    }
                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
