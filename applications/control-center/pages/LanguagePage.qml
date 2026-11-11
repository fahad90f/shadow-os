// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Region & Language")
    subtitle: qsTr("The language of the system and the formats of dates, numbers, currency and units.")

    actions: [
        NxButton {
            text: qsTr("Revert")
            visible: lang.dirty
            enabled: !lang.busy
            onClicked: lang.revert()
        },
        NxButton {
            text: qsTr("Apply")
            kind: "primary"
            visible: lang.dirty
            busy: lang.busy
            enabled: lang.available && !lang.busy
            onClicked: lang.apply()
        }
    ]

    LanguageBackend {
        id: lang
        onFailed: (message) => page.ApplicationWindow.window.toaster.show(message, "danger")
        onApplied: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }

    NxBanner {
        visible: lang.ready && !lang.available
        kind: "warning"
        title: qsTr("Language settings cannot be changed")
        message: lang.source !== "" ? qsTr("%1 The values below were read from %2.").arg(lang.statusMessage).arg(lang.source)
                                    : qsTr("%1 No locale configuration file was found either.").arg(lang.statusMessage)
    }
    NxBanner {
        kind: "info"
        title: lang.loginPending ? qsTr("Log out and back in to use the new settings")
                                 : qsTr("Changes take effect at your next login")
        message: lang.loginPending
            ? qsTr("The system is set to %1, but this session was started with %2. Running applications keep the language they started with.")
                  .arg(lang.systemLanguage).arg(lang.sessionLanguage)
            : qsTr("The language and formats are set for the whole computer. Applications that are already running, including this one, keep the settings they started with until you log out and back in.")
    }
    NxBanner {
        visible: lang.busy || (lang.dirty && lang.toGenerate.length > 0)
        kind: lang.busy ? "info" : "warning"
        title: lang.busy ? lang.busyText : qsTr("%1 must be installed first").arg(lang.toGenerate.join(", "))
        message: lang.busy ? "" : lang.helperAvailable
            ? qsTr("“Apply” generates it on this computer (locale-gen) before switching. You are asked for an administrator password; generating takes up to a minute.")
            : qsTr("The helper that generates locales (/usr/libexec/nexus/language-helper, run through pkexec) is not installed, so this selection cannot be applied.")
    }

    NxSection {
        title: qsTr("Language")
        NxRow {
            icon: "language"
            title: lang.language !== "" ? lang.displayName(lang.language) : qsTr("Not set")
            subtitle: {
                const parts = []
                if (lang.language !== "") parts.push(lang.language)
                if (lang.language !== lang.systemLanguage)
                    parts.push(lang.systemLanguage !== "" ? qsTr("currently %1").arg(lang.systemLanguage) : qsTr("currently not set"))
                return parts.join("  ·  ")
            }
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                visible: lang.language !== "" && !lang.isGenerated(lang.language)
                text: qsTr("Not installed yet")
                kind: "warning"
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Change…")
                enabled: !lang.busy
                onClicked: picker.openFor("LANG", qsTr("Choose the language"), lang.language)
            }
        }
    }

    NxSection {
        title: qsTr("Formats")
        description: qsTr("Each format follows the language unless a different region is chosen. The examples are produced with the selected region's rules.")

        Repeater {
            model: lang.formats
            NxRow {
                id: formatRow
                required property var modelData
                icon: modelData.category === "LC_TIME" ? "calendar"
                    : modelData.category === "LC_NUMERIC" ? "hash"
                    : modelData.category === "LC_MONETARY" ? "database" : "gauge"
                title: modelData.title
                subtitle: {
                    const parts = []
                    if (modelData.example !== "") parts.push(modelData.example)
                    parts.push(modelData.value === "" ? qsTr("Same as the language") : modelData.effectiveName + " — " + modelData.value)
                    return parts.join("  ·  ")
                }
                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: formatRow.modelData.value !== "" && !lang.isGenerated(formatRow.modelData.value)
                    text: qsTr("Not installed yet")
                    kind: "warning"
                }
                NxIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: formatRow.modelData.value !== ""
                    enabled: !lang.busy
                    iconName: "undo"
                    tooltip: qsTr("Use the format of the language for %1").arg(formatRow.modelData.title)
                    onClicked: lang.selectFormat(formatRow.modelData.category, "")
                }
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Change…")
                    enabled: !lang.busy
                    onClicked: picker.openFor(formatRow.modelData.category,
                                              qsTr("Region for %1").arg(formatRow.modelData.title), formatRow.modelData.effective)
                }
            }
        }
    }

    Label {
        Layout.fillWidth: true
        visible: !lang.localeListComplete
        text: qsTr("The list of locales glibc supports (/usr/share/i18n/SUPPORTED, package locales) is not installed; only the locales that are already generated are offered.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }

    // ---- locale picker ------------------------------------------------------------
    Dialog {
        id: picker
        width: Math.min(540, page.width - Theme.spaceXxl)
        height: Math.min(620, page.height - Theme.spaceXxl)
        footer: null

        property string target: "LANG"
        property string current: ""
        readonly property var matches: {
            const needle = pickerSearch.text.trim().toLowerCase()
            if (needle === "") return lang.locales
            return lang.locales.filter(l => l.name.toLowerCase().indexOf(needle) >= 0
                                            || l.englishName.toLowerCase().indexOf(needle) >= 0
                                            || l.id.toLowerCase().indexOf(needle) >= 0)
        }

        function openFor(category, heading, value) {
            target = category
            title = heading
            current = value
            open()
        }
        onOpened: {
            pickerSearch.text = ""
            pickerSearch.forceActiveFocus()
            for (let i = 0; i < lang.locales.length; i++) {
                if (lang.locales[i].id === current) { pickerList.positionViewAtIndex(i, ListView.Center); break }
            }
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            NxSearchField {
                id: pickerSearch
                Layout.fillWidth: true
                placeholderText: qsTr("Search by language, country or locale name")
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
                    id: pickerList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceXs
                    model: picker.matches
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar { }

                    delegate: ItemDelegate {
                        id: localeDelegate
                        required property var modelData
                        width: ListView.view.width
                        height: Theme.rowHeightLarge
                        highlighted: modelData.id === picker.current
                        Accessible.name: modelData.englishName + " " + modelData.id
                        onClicked: {
                            if (picker.target === "LANG") lang.selectLanguage(modelData.id)
                            else lang.selectFormat(picker.target, modelData.id)
                            picker.close()
                        }
                        contentItem: RowLayout {
                            spacing: Theme.spaceMd
                            NxIcon {
                                name: localeDelegate.modelData.id === picker.current ? "check" : "globe"
                                size: Theme.iconSmall
                                color: localeDelegate.modelData.id === picker.current ? Theme.accent : Theme.textMuted
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    Layout.fillWidth: true
                                    text: localeDelegate.modelData.name
                                    font.weight: Theme.weightMedium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: localeDelegate.modelData.englishName === localeDelegate.modelData.name
                                        ? localeDelegate.modelData.id
                                        : localeDelegate.modelData.englishName + "  ·  " + localeDelegate.modelData.id
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSizeSmall
                                    elide: Text.ElideRight
                                }
                            }
                            NxBadge {
                                visible: !localeDelegate.modelData.generated
                                text: qsTr("Not installed yet")
                                kind: "neutral"
                                dot: false
                            }
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - Theme.spaceXl * 2
                    visible: pickerList.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: lang.locales.length === 0 ? qsTr("No locales were found on this system.")
                                                    : qsTr("No locale matches “%1”.").arg(pickerSearch.text)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: picker.close() }
            }
        }
    }
}
