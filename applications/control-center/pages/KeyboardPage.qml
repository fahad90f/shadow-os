// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Keyboard")
    subtitle: qsTr("Layouts, layout switching, special keys and key repeat.")

    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) {
            if (items[i].value === value) return i
        }
        return -1
    }

    function applyRepeat() {
        const delay = Math.round(delaySlider.value)
        const rate = Math.round(rateSlider.value)
        if (delay === kb.repeatDelay && rate === kb.repeatRate && kb.repeatConfigured) return
        kb.setRepeat(delay, rate)
        delaySlider.value = Qt.binding(() => kb.repeatDelay)
        rateSlider.value = Qt.binding(() => kb.repeatRate)
    }

    KeyboardBackend {
        id: kb
        onFailed: (message) => page.ApplicationWindow.window.toaster.show(message, "danger")
    }

    NxCard {
        visible: kb.ready && !kb.available
        Layout.fillWidth: true
        implicitHeight: 300
        NxEmptyState {
            anchors.fill: parent
            icon: "keyboard"
            tone: "warning"
            title: kb.state === "unsupported-build" ? qsTr("Keyboard settings are not available in this build")
                                                    : qsTr("The keyboard layout catalogue is missing")
            message: kb.statusMessage
        }
    }

    NxBanner {
        visible: kb.available && kb.ready && !kb.compositorRunning
        kind: "info"
        title: qsTr("KWin is not running in this session")
        message: qsTr("Changes are saved to kxkbrc and kcminputrc and apply the next time a NEXUS desktop session starts.")
    }

    NxSection {
        visible: kb.available
        title: qsTr("Layouts")
        description: kb.layoutCount >= kb.maxLayouts
            ? qsTr("XKB supports at most %1 layouts at a time. Remove one to add another.").arg(kb.maxLayouts)
            : qsTr("The first layout is the default. Up to %1 layouts can be used side by side.").arg(kb.maxLayouts)
        headerActions: [
            NxButton {
                text: qsTr("Add layout")
                iconName: "plus"
                enabled: kb.layoutCount < kb.maxLayouts
                onClicked: addDialog.open()
            }
        ]

        NxRow {
            icon: "globe"
            title: qsTr("System default")
            subtitle: kb.systemLayoutSource !== ""
                ? qsTr("Used when no layout is configured here. Reported by %1.").arg(kb.systemLayoutSource)
                : qsTr("Neither systemd-localed nor /etc/default/keyboard reports a system keyboard layout.")
            value: kb.systemLayout !== "" ? kb.systemLayout : qsTr("Unknown")
        }

        Repeater {
            model: kb.layouts
            NxRow {
                id: layoutRow
                required property string name
                required property string layoutCode
                required property string variantCode
                required property int position
                required property bool active

                icon: "keyboard"
                iconColor: active ? Theme.accent : Theme.textSecondary
                title: name
                subtitle: variantCode !== "" ? layoutCode + " · " + variantCode : layoutCode

                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: layoutRow.active
                    text: qsTr("In use")
                    kind: "success"
                }
                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: layoutRow.position === 0 && kb.layoutCount > 1
                    text: qsTr("Default")
                    dot: false
                }
                NxIconButton {
                    iconName: "arrow-up"
                    tooltip: qsTr("Move %1 up").arg(layoutRow.name)
                    enabled: layoutRow.position > 0
                    onClicked: kb.moveLayout(layoutRow.position, layoutRow.position - 1)
                }
                NxIconButton {
                    iconName: "arrow-down"
                    tooltip: qsTr("Move %1 down").arg(layoutRow.name)
                    enabled: layoutRow.position < kb.layoutCount - 1
                    onClicked: kb.moveLayout(layoutRow.position, layoutRow.position + 1)
                }
                NxIconButton {
                    iconName: "trash"
                    tooltip: qsTr("Remove %1").arg(layoutRow.name)
                    onClicked: kb.removeLayout(layoutRow.position)
                }
            }
        }

        NxRow {
            visible: kb.layoutCount === 0
            icon: "info"
            title: qsTr("No layout configured")
            subtitle: kb.activeLayouts.length > 0
                ? qsTr("The session uses the system default: %1.").arg(kb.activeLayouts.join(", "))
                : qsTr("The session uses the system default layout.")
        }
    }

    NxSection {
        visible: kb.available
        title: qsTr("Switching and special keys")
        description: kb.otherOptions.length > 0
            ? qsTr("Other XKB options set outside this page are kept: %1").arg(kb.otherOptions.join(", "))
            : ""

        NxRow {
            icon: "language"
            title: qsTr("Switch layouts with")
            subtitle: qsTr("Key combination that selects the next layout")
            ComboBox {
                id: switchCombo
                width: 280
                model: kb.switchOptions
                textRole: "text"
                valueRole: "value"
                currentIndex: page.indexOfValue(kb.switchOptions, kb.switchOption)
                displayText: currentIndex < 0 && kb.switchOption !== "" ? kb.switchOption : currentText
                Accessible.name: qsTr("Switch layouts with")
                onActivated: {
                    kb.setSwitchOption(currentValue)
                    currentIndex = Qt.binding(() => page.indexOfValue(kb.switchOptions, kb.switchOption))
                }
            }
        }
        NxRow {
            icon: "type"
            title: qsTr("Compose key")
            subtitle: qsTr("Starts a sequence that types characters such as é, ß or “")
            ComboBox {
                width: 280
                model: kb.composeOptions
                textRole: "text"
                valueRole: "value"
                currentIndex: page.indexOfValue(kb.composeOptions, kb.composeOption)
                displayText: currentIndex < 0 && kb.composeOption !== "" ? kb.composeOption : currentText
                Accessible.name: qsTr("Compose key")
                onActivated: {
                    kb.setComposeOption(currentValue)
                    currentIndex = Qt.binding(() => page.indexOfValue(kb.composeOptions, kb.composeOption))
                }
            }
        }
        NxRow {
            icon: "arrow-up"
            title: qsTr("Caps Lock behaviour")
            ComboBox {
                width: 280
                model: kb.capsOptions
                textRole: "text"
                valueRole: "value"
                currentIndex: page.indexOfValue(kb.capsOptions, kb.capsOption)
                displayText: currentIndex < 0 && kb.capsOption !== "" ? kb.capsOption : currentText
                Accessible.name: qsTr("Caps Lock behaviour")
                onActivated: {
                    kb.setCapsOption(currentValue)
                    currentIndex = Qt.binding(() => page.indexOfValue(kb.capsOptions, kb.capsOption))
                }
            }
        }
    }

    NxSection {
        visible: kb.available
        title: qsTr("Key repeat")
        description: kb.repeatConfigured ? "" : qsTr("Not set yet — KWin uses its defaults until you change a value.")

        NxRow {
            icon: "timer"
            title: qsTr("Delay")
            subtitle: qsTr("How long a key is held before it starts repeating")
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%1 ms").arg(Math.round(delaySlider.value))
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: delaySlider
                anchors.verticalCenter: parent.verticalCenter
                width: 240
                from: 100
                to: 2000
                stepSize: 10
                snapMode: Slider.SnapAlways
                value: kb.repeatDelay
                Accessible.name: qsTr("Repeat delay in milliseconds")
                onPressedChanged: if (!pressed) page.applyRepeat()
                onMoved: if (!pressed) page.applyRepeat()
            }
        }
        NxRow {
            icon: "gauge"
            title: qsTr("Rate")
            subtitle: qsTr("How many characters per second a held key produces")
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%1 / s").arg(Math.round(rateSlider.value))
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: rateSlider
                anchors.verticalCenter: parent.verticalCenter
                width: 240
                from: 1
                to: 100
                stepSize: 1
                snapMode: Slider.SnapAlways
                value: kb.repeatRate
                Accessible.name: qsTr("Repeat rate in characters per second")
                onPressedChanged: if (!pressed) page.applyRepeat()
                onMoved: if (!pressed) page.applyRepeat()
            }
        }
    }

    NxSection {
        visible: kb.available
        title: qsTr("Test area")
        description: qsTr("Type here to try the active layout, the Compose key and key repeat.")
        Item {
            Layout.fillWidth: true
            implicitHeight: testField.implicitHeight + Theme.spaceMd * 2
            TextField {
                id: testField
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                placeholderText: qsTr("Type to test")
                Accessible.name: qsTr("Keyboard test field")
            }
        }
    }

    NxSection {
        title: qsTr("Desktop shortcuts")
        description: kb.shortcutsState === "ok"
            ? qsTr("Global shortcuts registered with the desktop's shortcut service (KGlobalAccel): %1 of %2 actions have a key. "
                   + "The list is read-only — shortcuts cannot be changed here.").arg(kb.assignedShortcutCount).arg(kb.totalShortcutCount)
            : ""
        headerActions: [
            NxIconButton {
                iconName: "refresh"
                tooltip: qsTr("Read the shortcuts again")
                onClicked: kb.refreshShortcuts()
            }
        ]

        NxRow {
            visible: kb.shortcutsState !== "ok"
            icon: kb.shortcutsState === "loading" ? "clock" : "alert-triangle"
            title: kb.shortcutsState === "loading" ? qsTr("Reading the shortcuts…")
                                                   : qsTr("The desktop shortcuts cannot be listed")
            subtitle: kb.shortcutsStatus
        }
        NxBanner {
            visible: kb.shortcutsState === "ok" && kb.shortcutsStatus !== ""
            Layout.fillWidth: true
            Layout.margins: Theme.spaceMd
            kind: "warning"
            title: qsTr("The list is incomplete")
            message: kb.shortcutsStatus
        }

        Item {
            visible: kb.shortcutsState === "ok"
            Layout.fillWidth: true
            implicitHeight: shortcutTools.implicitHeight + Theme.spaceMd * 2
            RowLayout {
                id: shortcutTools
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceLg
                NxSearchField {
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search by action, application or key")
                    Accessible.name: qsTr("Search shortcuts")
                    onTextChanged: kb.shortcutFilter = text
                }
                CheckBox {
                    text: qsTr("Show actions without a key")
                    checked: kb.showUnassignedShortcuts
                    onToggled: kb.showUnassignedShortcuts = checked
                }
            }
        }

        Repeater {
            model: kb.shortcutsState === "ok" ? kb.shortcuts : null
            NxRow {
                id: shortcutRow
                required property string name
                required property string componentName
                required property string keys
                required property bool assigned
                required property bool custom

                icon: "keyboard"
                iconColor: assigned ? Theme.textSecondary : Theme.textDisabled
                title: name
                subtitle: custom && assigned ? qsTr("%1  ·  changed from its default").arg(componentName) : componentName

                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.spaceXs
                    Repeater {
                        model: shortcutRow.assigned ? shortcutRow.keys.split(", ") : []
                        Rectangle {
                            required property string modelData
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: keyLabel.implicitWidth + Theme.spaceMd * 2
                            implicitHeight: keyLabel.implicitHeight + Theme.spaceXs * 2
                            radius: Theme.radiusSm
                            color: Theme.bgInput
                            border.width: Theme.borderWidth
                            border.color: Theme.border
                            Label {
                                id: keyLabel
                                anchors.centerIn: parent
                                text: parent.modelData
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                    Label {
                        visible: !shortcutRow.assigned
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("No key")
                        color: Theme.textMuted
                    }
                }
            }
        }

        NxRow {
            visible: kb.shortcutsState === "ok" && kb.shortcutCount === 0
            icon: "info"
            title: kb.totalShortcutCount === 0 ? qsTr("No global shortcut is registered")
                 : kb.shortcutFilter.trim() !== "" ? qsTr("No shortcut matches “%1”").arg(kb.shortcutFilter.trim())
                 : qsTr("No action has a key assigned")
        }
    }

    Dialog {
        id: addDialog
        title: qsTr("Add a keyboard layout")
        width: Math.min(520, page.width - Theme.spaceXxl)
        height: Math.min(600, page.height - Theme.spaceXxl)
        footer: null

        property string selectedLayout: ""
        readonly property var variants: selectedLayout !== "" ? kb.variantsFor(selectedLayout) : []
        readonly property var matches: {
            const needle = search.text.trim().toLowerCase()
            if (needle === "") return kb.availableLayouts
            return kb.availableLayouts.filter(l => l.name.toLowerCase().indexOf(needle) >= 0
                                                   || l.layout.toLowerCase().indexOf(needle) >= 0)
        }

        onOpened: {
            search.text = ""
            selectedLayout = ""
            search.forceActiveFocus()
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            NxSearchField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search layouts by language or code")
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.bgInput
                radius: Theme.radiusSm
                border.width: Theme.borderWidth
                border.color: Theme.border
                clip: true

                ListView {
                    id: layoutList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceXs
                    model: addDialog.matches
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    delegate: ItemDelegate {
                        required property var modelData
                        width: ListView.view.width
                        text: modelData.name + "  (" + modelData.layout + ")"
                        highlighted: modelData.layout === addDialog.selectedLayout
                        onClicked: addDialog.selectedLayout = modelData.layout
                        onDoubleClicked: {
                            addDialog.selectedLayout = modelData.layout
                            if (kb.addLayout(modelData.layout, "")) addDialog.close()
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    visible: layoutList.count === 0
                    text: qsTr("No layout matches “%1”").arg(search.text)
                    color: Theme.textMuted
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Label {
                    text: qsTr("Variant")
                    color: addDialog.selectedLayout !== "" ? Theme.textSecondary : Theme.textDisabled
                }
                ComboBox {
                    id: variantCombo
                    Layout.fillWidth: true
                    enabled: addDialog.selectedLayout !== ""
                    model: addDialog.variants
                    textRole: "name"
                    valueRole: "variant"
                    displayText: addDialog.selectedLayout === "" ? qsTr("Choose a layout first") : currentText
                    Accessible.name: qsTr("Layout variant")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: addDialog.close()
                }
                NxButton {
                    text: qsTr("Add")
                    kind: "primary"
                    enabled: addDialog.selectedLayout !== ""
                    onClicked: {
                        const variant = variantCombo.currentIndex >= 0 ? variantCombo.currentValue : ""
                        if (kb.addLayout(addDialog.selectedLayout, variant)) addDialog.close()
                    }
                }
            }
        }
    }
}
