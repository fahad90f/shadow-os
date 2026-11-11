// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Date & Time")
    subtitle: qsTr("Clock, time zone and how the time is shown in the panel.")

    // Bumped when a change is refused, so controls return to the real state.
    property int revision: 0
    property bool manualEdited: false

    function fillManualFields() {
        if (manualEdited) return
        dateField.text = dt.isoDate
        timeField.text = dt.isoTime
    }

    DateTimeBackend {
        id: dt
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onChanged: (message) => {
            page.manualEdited = false
            page.fillManualFields()
            page.ApplicationWindow.window.toaster.show(message, "success")
        }
        onStateChanged: page.fillManualFields()
    }
    Component.onCompleted: fillManualFields()

    NxBanner {
        visible: dt.ready && !dt.available
        kind: "warning"
        title: qsTr("Date and time settings cannot be changed")
        message: dt.statusMessage
    }

    // ---- now ---------------------------------------------------------------------
    NxCard {
        Layout.fillWidth: true
        implicitHeight: 116

        RowLayout {
            anchors.fill: parent
            spacing: Theme.spaceXl

            Rectangle {
                implicitWidth: 64
                implicitHeight: 64
                radius: 32
                color: Theme.accentFill
                NxIcon { anchors.centerIn: parent; name: "clock"; size: 30; color: Theme.accent }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Label {
                    Layout.fillWidth: true
                    text: dt.currentTime
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Theme.weightSemibold
                    font.features: { "tnum": 1 }
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: dt.currentDate
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("%1  ·  %2").arg(dt.timezone !== "" ? dt.timezone : qsTr("Unknown time zone")).arg(dt.utcOffsetText)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
        }
    }

    // ---- clock ---------------------------------------------------------------------
    NxSection {
        title: qsTr("Clock")

        NxRow {
            icon: "globe"
            title: qsTr("Set the time automatically")
            subtitle: !dt.available ? qsTr("Not known without the time service.")
                    : !dt.canNtp ? qsTr("No network time service (such as systemd-timesyncd or chrony) is installed.")
                    : !dt.ntp ? qsTr("Off — the clock runs on its own.")
                    : dt.ntpSynchronized ? qsTr("On — the clock is synchronised with a time server.")
                                         : qsTr("On — not synchronised yet.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: dt.available && dt.canNtp && !dt.busy
                checked: (page.revision, dt.ntp)
                Accessible.name: qsTr("Set the time automatically")
                onToggled: {
                    dt.setNtp(checked)
                    checked = Qt.binding(() => (page.revision, dt.ntp))
                }
            }
        }
        NxRow {
            visible: dt.available
            icon: "edit"
            title: qsTr("Set date and time")
            subtitle: dt.ntp ? qsTr("Switch off automatic time to set the clock by hand.")
                             : manualProblem.text !== "" ? "" : qsTr("Year-month-day and 24-hour time in the time zone %1.").arg(dt.timezone)
            TextField {
                id: dateField
                anchors.verticalCenter: parent.verticalCenter
                width: 118
                enabled: !dt.ntp && !dt.busy
                font.family: Theme.fontMono
                placeholderText: "2026-01-31"
                inputMethodHints: Qt.ImhDate
                Accessible.name: qsTr("Date (year-month-day)")
                onTextEdited: { page.manualEdited = true; manualProblem.text = "" }
                onAccepted: setButton.clicked()
            }
            TextField {
                id: timeField
                anchors.verticalCenter: parent.verticalCenter
                width: 96
                enabled: !dt.ntp && !dt.busy
                font.family: Theme.fontMono
                placeholderText: "14:30:00"
                inputMethodHints: Qt.ImhTime
                Accessible.name: qsTr("Time (hours:minutes:seconds)")
                onTextEdited: { page.manualEdited = true; manualProblem.text = "" }
                onAccepted: setButton.clicked()
            }
            NxButton {
                id: setButton
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Set")
                kind: "primary"
                busy: dt.busy
                enabled: !dt.ntp && !dt.busy && page.manualEdited
                onClicked: manualProblem.text = dt.setDateTime(dateField.text, timeField.text)
            }
        }
        Label {
            id: manualProblem
            visible: text !== ""
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            color: Theme.danger
            wrapMode: Text.WordWrap
        }
        NxRow {
            icon: "cpu"
            title: qsTr("Hardware clock")
            subtitle: !dt.localRtcKnown ? qsTr("Cannot be determined without the time service; the Linux default is UTC.")
                    : dt.localRtc ? qsTr("%1. Local time in the hardware clock causes wrong times around clock changes; it is usually set for dual boot with Windows.").arg(dt.localRtcSource)
                                  : dt.localRtcSource
            iconColor: dt.localRtcKnown && dt.localRtc ? Theme.warning : Theme.textSecondary
            value: !dt.localRtcKnown ? qsTr("Unknown") : dt.localRtc ? qsTr("Local time") : qsTr("UTC")
        }
    }

    // ---- time zone -----------------------------------------------------------------
    NxSection {
        title: qsTr("Time zone")
        description: qsTr("%1 zones from %2.").arg(dt.zoneCount).arg(dt.zoneSource)

        NxRow {
            icon: "flag"
            title: dt.timezoneLabel
            subtitle: dt.timezone
            value: dt.utcOffsetText
        }
        Item {
            Layout.fillWidth: true
            implicitHeight: zoneBody.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: zoneBody
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    NxSearchField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("Search by city, country or UTC offset")
                        onTextChanged: dt.filter = text
                    }
                    ComboBox {
                        Layout.preferredWidth: 170
                        model: [qsTr("All regions")].concat(dt.regions)
                        Accessible.name: qsTr("Region")
                        onActivated: (index) => dt.region = index === 0 ? "" : dt.regions[index - 1]
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 296
                    color: Theme.bgInput
                    radius: Theme.radiusSm
                    border.width: Theme.borderWidth
                    border.color: Theme.border
                    clip: true

                    ListView {
                        id: zoneList
                        anchors.fill: parent
                        anchors.margins: Theme.spaceXs
                        model: dt.zones
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        ScrollBar.vertical: ScrollBar { }

                        delegate: ItemDelegate {
                            id: zoneDelegate
                            required property var modelData
                            width: ListView.view.width
                            height: Theme.rowHeightLarge
                            enabled: dt.available && !dt.busy
                            highlighted: modelData.current
                            Accessible.name: modelData.id
                            onClicked: dt.setTimezone(modelData.id)
                            contentItem: RowLayout {
                                spacing: Theme.spaceMd
                                NxIcon {
                                    name: zoneDelegate.modelData.current ? "check" : "globe"
                                    size: Theme.iconSmall
                                    color: zoneDelegate.modelData.current ? Theme.accent : Theme.textMuted
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Label {
                                        Layout.fillWidth: true
                                        text: zoneDelegate.modelData.city
                                        font.weight: Theme.weightMedium
                                        color: zoneDelegate.enabled ? Theme.textPrimary : Theme.textSecondary
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: [zoneDelegate.modelData.countries, zoneDelegate.modelData.comment].filter(s => s !== "").join("  ·  ")
                                        visible: text !== ""
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontSizeSmall
                                        elide: Text.ElideRight
                                    }
                                }
                                Label {
                                    text: zoneDelegate.modelData.abbreviation
                                    visible: text !== "" && text.indexOf("UTC") !== 0 && text.indexOf("GMT") !== 0 && !/^[+-]/.test(text)
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                                Label {
                                    text: zoneDelegate.modelData.offsetText
                                    color: Theme.textSecondary
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fontSizeSmall
                                }
                            }
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: zoneList.count === 0
                        text: qsTr("No time zone matches the search.")
                        color: Theme.textMuted
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: dt.available ? qsTr("Choosing a zone changes it for the whole computer; you may be asked for an administrator password.")
                                       : qsTr("The list can be browsed, but the time zone cannot be changed without the time service.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // ---- panel clock ----------------------------------------------------------------
    NxSection {
        title: qsTr("Clock in the panel")
        description: qsTr("Stored in %1, which the shell follows.").arg(dt.settingsFile)
        NxRow {
            icon: "clock"
            title: qsTr("24-hour clock")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: dt.clock24h
                Accessible.name: qsTr("24-hour clock")
                onToggled: dt.setClock24h(checked)
            }
        }
        NxRow {
            icon: "timer"
            title: qsTr("Show seconds")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: dt.clockSeconds
                Accessible.name: qsTr("Show seconds")
                onToggled: dt.setClockSeconds(checked)
            }
        }
        NxRow {
            icon: "calendar"
            title: qsTr("Show the date")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: dt.clockDate
                Accessible.name: qsTr("Show the date")
                onToggled: dt.setClockDate(checked)
            }
        }
    }
}
