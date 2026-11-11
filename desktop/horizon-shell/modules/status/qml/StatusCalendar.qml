// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// The calendar popup under the panel clock: today's date, a month grid in
// the user's locale (first day of the week, names) with ISO 8601 week
// numbers, and the time zone the clock runs in.
//
// Keys: Left / Page Up and Right / Page Down change the month, Home returns
// to today.
PopupSurface {
    id: popup

    required property var shell

    readonly property var cal: status.calendar
    // The month shown.
    property int year: cal.todayYear
    property int month: cal.todayMonth
    readonly property bool showsToday: year === cal.todayYear && month === cal.todayMonth
    readonly property var weeks: {
        cal.todayDay; cal.todayMonth; cal.todayYear     // "today" moves at midnight
        return cal.weeks(year, month)
    }
    property date now: new Date()

    readonly property real weekColumnWidth: Theme.spaceXl + Theme.spaceXs
    readonly property real cellWidth: Math.floor((popupWidth - padding * 2 - weekColumnWidth) / 7)
    readonly property real cellHeight: Theme.controlHeight

    function showToday() {
        year = cal.todayYear
        month = cal.todayMonth
    }
    function stepMonth(delta) {
        let y = year, m = month + delta
        if (m < 1) { m = 12; y -= 1 }
        else if (m > 12) { m = 1; y += 1 }
        if (y < 1 || y > 9999) return
        year = y
        month = m
    }

    screen: shell.primaryScreen
    alignment: "center"
    popupWidth: 348
    scope: "nexus-calendar"

    Component.onCompleted: shell.registerOverlay("calendar", popup)
    onOpenChanged: if (open) {
        cal.refresh()
        now = new Date()
        showToday()
    }

    Timer {
        interval: 1000
        running: popup.open
        repeat: true
        onTriggered: popup.now = new Date()
    }

    ColumnLayout {
        width: parent.width
        spacing: Theme.spaceMd
        focus: true
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Left || event.key === Qt.Key_PageUp) popup.stepMonth(-1)
            else if (event.key === Qt.Key_Right || event.key === Qt.Key_PageDown) popup.stepMonth(1)
            else if (event.key === Qt.Key_Home) popup.showToday()
            else return
            event.accepted = true
        }

        // ---- today ----------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: popup.cal.todayWeekday
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeHeading
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: popup.cal.todayLongDate
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    elide: Text.ElideRight
                }
            }
            NxBadge {
                Layout.alignment: Qt.AlignTop
                text: qsTr("Week %1").arg(popup.cal.todayWeekNumber)
            }
        }

        NxDivider { Layout.fillWidth: true }

        // ---- month ----------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spaceXs
                text: popup.cal.monthTitle(popup.year, popup.month)
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            NxIconButton {
                iconName: "chevron-left"
                iconSize: Theme.iconSmall
                tooltip: qsTr("Previous month")
                onClicked: popup.stepMonth(-1)
            }
            NxIconButton {
                iconName: "dot"
                iconSize: Theme.iconSmall
                tooltip: qsTr("Back to today")
                enabled: !popup.showsToday
                onClicked: popup.showToday()
            }
            NxIconButton {
                iconName: "chevron-right"
                iconSize: Theme.iconSmall
                tooltip: qsTr("Next month")
                onClicked: popup.stepMonth(1)
            }
        }

        Column {
            Layout.fillWidth: true
            spacing: 0

            Row {
                Text {
                    width: popup.weekColumnWidth
                    height: popup.cellHeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: qsTr("Wk", "column header: week number")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeCaption
                    Accessible.name: qsTr("Week number")
                }
                Repeater {
                    model: popup.cal.dayNames
                    Text {
                        required property string modelData
                        required property int index
                        width: popup.cellWidth
                        height: popup.cellHeight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: modelData
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        // Some languages have no short day names.
                        fontSizeMode: Text.HorizontalFit
                        minimumPixelSize: Theme.fontSizeCaption - 4
                        font.weight: Theme.weightMedium
                        Accessible.name: popup.cal.longDayNames[index]
                    }
                }
            }
            Repeater {
                model: popup.weeks
                Row {
                    id: weekRow
                    required property var modelData
                    Text {
                        width: popup.weekColumnWidth
                        height: popup.cellHeight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: weekRow.modelData.week
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeCaption
                        Accessible.name: qsTr("Week %1").arg(weekRow.modelData.week)
                    }
                    Repeater {
                        model: weekRow.modelData.days
                        Item {
                            id: cell
                            required property var modelData
                            width: popup.cellWidth
                            height: popup.cellHeight
                            Accessible.role: Accessible.StaticText
                            Accessible.name: cell.modelData.today ? qsTr("Today, %1").arg(cell.modelData.label) : cell.modelData.label
                            Rectangle {
                                anchors.centerIn: parent
                                visible: cell.modelData.today
                                width: Math.min(parent.width, parent.height) - Theme.spaceXs
                                height: width
                                radius: width / 2
                                color: Theme.accent
                            }
                            Text {
                                anchors.centerIn: parent
                                text: cell.modelData.day
                                color: cell.modelData.today ? Theme.textOnAccent
                                    : !cell.modelData.inMonth ? Theme.textDisabled
                                    : cell.modelData.weekend ? Theme.textSecondary : Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: cell.modelData.today ? Theme.weightBold : Theme.weightRegular
                            }
                        }
                    }
                }
            }
        }

        NxDivider { Layout.fillWidth: true }

        // ---- time and time zone ---------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon { name: "clock"; size: Theme.iconSmall; color: Theme.textSecondary }
            Text {
                text: Qt.formatTime(popup.now, shellSettings.clock24h ? "HH:mm:ss" : "h:mm:ss AP")
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideLeft
                // "Europe/Berlin · CET (UTC+01:00)"; the abbreviation is left
                // out where it only repeats the offset.
                text: {
                    const zone = popup.cal.timeZoneId, abbr = popup.cal.timeZoneAbbreviation, offset = popup.cal.utcOffset
                    const detail = abbr !== "" && abbr !== offset && abbr !== zone && !/^[+-]?\d/.test(abbr)
                        ? abbr + " (" + offset + ")" : offset
                    return zone !== "" && zone !== detail ? zone + "  ·  " + detail : detail
                }
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }
}
