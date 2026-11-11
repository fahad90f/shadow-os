// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    title: qsTr("Keyboard")
    subtitle: qsTr("Choose the keyboard layout of the installed system.")
    ready: Keyboard.problem !== "" || Keyboard.hasVariant(wiz.layout, wiz.variant)
    blockReason: ready ? "" : qsTr("Choose a keyboard layout.")

    readonly property bool sessionUsesChoice: Keyboard.sessionKnown && Keyboard.sessionMatches(wiz.layout, wiz.variant)

    function entered() {
        if (!wiz.layoutChosen && Keyboard.problem === "") {
            const s = Keyboard.suggestionForLocale(wiz.locale)
            if (s.layout !== undefined && Keyboard.hasLayout(s.layout)) {
                wiz.layout = s.layout
                wiz.variant = s.variant || ""
            }
        }
        layouts.positionAtSelection()
        variants.positionAtSelection()
    }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: Keyboard.problem !== ""
        icon: "keyboard"
        tone: "warning"
        title: qsTr("The list of keyboard layouts is unavailable")
        message: Keyboard.problem
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: Keyboard.problem === ""
        spacing: Theme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 10
            spacing: Theme.spaceSm
            NxSearchField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search layouts")
            }
            PickList {
                id: layouts
                model: Keyboard.filteredLayouts(search.text)
                keyRole: "code"
                titleRole: "name"
                trailingRole: "code"
                selectedKey: page.wiz.layout
                accessibleName: qsTr("Keyboard layout")
                emptyText: qsTr("No layout matches “%1”.").arg(search.text)
                onPicked: (key) => {
                    if (page.wiz.layout !== key) { page.wiz.layout = key; page.wiz.variant = "" }
                    page.wiz.layoutChosen = true
                }
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredWidth: 10
            spacing: Theme.spaceSm
            Text {
                Layout.fillWidth: true
                Layout.preferredHeight: search.height
                verticalAlignment: Text.AlignVCenter
                text: qsTr("Variant")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightMedium
            }
            PickList {
                id: variants
                model: Keyboard.variantsFor(page.wiz.layout)
                keyRole: "code"
                titleRole: "name"
                trailingRole: "code"
                selectedKey: page.wiz.variant
                accessibleName: qsTr("Layout variant")
                onPicked: (key) => { page.wiz.variant = key; page.wiz.layoutChosen = true }
            }
        }
    }

    NxCard {
        Layout.fillWidth: true
        visible: Keyboard.problem === ""
        implicitHeight: tryColumn.implicitHeight + padding * 2
        ColumnLayout {
            id: tryColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.spaceSm
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                TextField {
                    id: tryField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Type here to try the keyboard")
                    Accessible.name: placeholderText
                    selectByMouse: true
                }
                NxButton {
                    visible: Keyboard.canApply
                    text: qsTr("Use in this session")
                    iconName: "keyboard"
                    busy: Keyboard.applyState === "applying"
                    enabled: !page.sessionUsesChoice
                    onClicked: Keyboard.applyToSession(page.wiz.layout, page.wiz.variant)
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                color: Keyboard.applyState === "failed" && Keyboard.canApply ? Theme.warning : Theme.textMuted
                text: {
                    const chosen = Keyboard.describe(page.wiz.layout, page.wiz.variant)
                    if (page.sessionUsesChoice)
                        return qsTr("This session types with %1, the layout chosen above (reported by the window manager).").arg(Keyboard.sessionLayoutName)
                    if (Keyboard.canApply) {
                        if (Keyboard.applyState === "applying" || Keyboard.applyState === "failed")
                            return Keyboard.applyMessage
                        return qsTr("This session still types with %1. The field above therefore does not show %2 until you choose “Use in this session”.")
                                .arg(Keyboard.sessionLayoutName).arg(chosen)
                    }
                    return (Keyboard.sessionKnown
                            ? qsTr("Typing here uses the layout of the running session (%1), not necessarily %2.").arg(Keyboard.sessionLayoutName).arg(chosen)
                            : qsTr("Typing here uses whatever layout the running session has, not necessarily %1.").arg(chosen))
                           + " " + Keyboard.cannotApplyReason
                }
            }
        }
    }
}
