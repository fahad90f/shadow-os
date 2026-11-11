// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    title: qsTr("Welcome to NEXUS OS")
    subtitle: qsTr("This assistant installs NEXUS OS on this computer. Nothing is written to any disk until you "
                 + "have seen and confirmed the exact plan at the end. First choose the language of the installed system.")
    ready: Locales.problem !== "" || Locales.contains(wiz.locale)
    blockReason: ready ? "" : qsTr("Choose a language.")

    function entered() { list.positionAtSelection() }

    NxSearchField {
        id: search
        Layout.fillWidth: true
        visible: Locales.problem === ""
        placeholderText: qsTr("Search %1 languages and regions").arg(Locales.count)
        Keys.onDownPressed: list.focusList()
    }

    PickList {
        id: list
        visible: Locales.problem === ""
        model: Locales.filtered(search.text)
        keyRole: "code"
        titleRole: "native"
        subtitleRole: "english"
        trailingRole: "code"
        selectedKey: page.wiz.locale
        accessibleName: qsTr("Language of the installed system")
        emptyText: qsTr("No language matches “%1”.").arg(search.text)
        onPicked: (key) => page.wiz.locale = key
    }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: Locales.problem !== ""
        icon: "language"
        tone: "warning"
        title: qsTr("The list of languages is unavailable")
        message: Locales.problem + " " + qsTr("The installed system will use %1; the language can be changed afterwards in the Control Center.").arg(page.wiz.locale)
    }

    Text {
        readonly property var rejected: Locales.unsupportedMatching(search.text)
        Layout.fillWidth: true
        visible: Locales.problem === "" && rejected.length > 0
        text: visible ? qsTr("%1 more matching locale(s) exist on this system but cannot be set up by the installation engine (for example %2). They can be configured after installation.")
                            .arg(rejected.length).arg(rejected[0].english + " — " + rejected[0].code) : ""
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("The installer itself is shown in English. Selected: %1 (%2).").arg(Locales.displayName(page.wiz.locale)).arg(page.wiz.locale)
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }
}
