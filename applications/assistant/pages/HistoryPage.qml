// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// History & undo: everything Shadow changed on disk, as a trail of stars —
// one per action, linked by a thin line, grouped by day (local time), each
// with Undo. Undo failures arrive as whispers (agent.errorMessage).
Item {
    id: page
    required property StageModel stage
    readonly property bool wide: width >= 900

    function reload() {
        entries.clear()
        const log = stage.agent.actionLog()
        for (let i = 0; i < log.length; ++i) {
            const e = log[i]
            const d = new Date(e.time)
            entries.append({ eid: String(e.id), op: e.op || "", path: e.path || "", summary: e.summary || "",
                             undone: e.undone === true, day: dayLabel(d),
                             clock: isNaN(d.getTime()) ? String(e.time) : Qt.formatTime(d, "HH:mm") })
        }
    }
    function dayLabel(d: date): string {
        if (isNaN(d.getTime()))
            return ""
        const now = new Date()
        const a = new Date(now.getFullYear(), now.getMonth(), now.getDate())
        const b = new Date(d.getFullYear(), d.getMonth(), d.getDate())
        const days = Math.round((a - b) / 86400000)
        return days === 0 ? qsTr("TODAY") : days === 1 ? qsTr("YESTERDAY") : Qt.formatDate(d, "d MMMM yyyy").toUpperCase()
    }
    function dayAt(i: int): string {
        return i >= 0 && i < entries.count ? entries.get(i).day : ""
    }
    function verb(op: string): string {
        return op === "delete_path" ? qsTr("Deleted") : op === "move_path" ? qsTr("Moved")
             : op === "copy_path" ? qsTr("Copied") : op === "make_directory" ? qsTr("Created a folder")
             : op === "write_code" ? qsTr("Wrote code") : qsTr("Wrote a file")
    }
    function icon(op: string): string {
        return op === "delete_path" ? "trash" : op === "move_path" ? "arrow-right" : op === "copy_path" ? "copy"
             : op === "make_directory" ? "folder" : op === "write_code" ? "code" : "file-text"
    }
    Component.onCompleted: reload()
    onVisibleChanged: if (visible) reload()
    Connections {
        target: page.stage.agent
        function onActionLogChanged() { page.reload() }
    }
    ListModel { id: entries }

    Flickable {
        id: flick
        x: 0
        y: 72
        width: parent.width
        height: parent.height - y
        contentHeight: col.implicitHeight + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: PeScrollBar {}

        ColumnLayout {
            id: col
            // the same column as Settings' (centred, clear of her face and the index)
            x: page.wide ? Math.max(232, Math.round((flick.width - width) / 2)) : 16
            width: Math.min(720, flick.width - (page.wide ? 272 : 32))
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 12
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Everything Shadow created, changed, moved or deleted on your disk. Undo puts back what was there before.")
                    font.family: Pe.sans; font.pixelSize: Pe.ui; lineHeight: 1.3
                    color: Pe.inkSoft
                }
                PeButton {
                    kind: "ghost"; compact: true
                    iconName: "refresh"
                    text: qsTr("Refresh")
                    onClicked: page.reload()
                }
            }

            // empty
            ColumnLayout {
                visible: entries.count === 0
                Layout.fillWidth: true
                Layout.topMargin: 80
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Nothing to undo yet.")
                    font.family: Pe.sans; font.pixelSize: 22; font.weight: Font.Light
                    color: Pe.ink
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    Layout.leftMargin: Math.max(0, (col.width - 420) / 2)
                    Layout.rightMargin: Math.max(0, (col.width - 420) / 2)
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: qsTr("When she writes, moves or deletes a file, it appears here and you can take it back.")
                    font.family: Pe.sans; font.pixelSize: Pe.ui
                    color: Pe.inkMute
                }
            }

            Repeater {
                model: entries
                delegate: Item {
                    id: entry
                    required property int index
                    required property string eid
                    required property string op
                    required property string path
                    required property string summary
                    required property bool undone
                    required property string day
                    required property string clock
                    readonly property bool newDay: index <= 0 || index > entries.count || page.dayAt(index - 1) !== day
                    readonly property bool lastOfDay: index + 1 >= entries.count || page.dayAt(index + 1) !== day
                    readonly property real topY: newDay ? 46 : 0
                    Layout.fillWidth: true
                    implicitHeight: topY + 62
                    Accessible.role: Accessible.ListItem
                    Accessible.name: (summary !== "" ? summary : page.verb(op)) + ", " + clock + (undone ? ", " + qsTr("undone") : "")

                    Text {
                        textFormat: Text.PlainText
                        visible: entry.newDay
                        y: 22
                        text: entry.day
                        font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Bold
                        font.letterSpacing: Pe.labelTracking
                        color: Pe.inkMute
                    }
                    // the trail
                    Rectangle {
                        x: 59
                        y: entry.topY + (entry.newDay ? 22 : 0)
                        width: 1
                        height: entry.lastOfDay ? Math.max(0, 22 - (entry.newDay ? 22 : 0) + 1) : entry.height - y
                        color: Pe.line
                        visible: !(entry.newDay && entry.lastOfDay)
                    }
                    Text {
                        textFormat: Text.PlainText
                        y: entry.topY + 14
                        width: 44
                        text: entry.clock
                        font.family: Pe.mono; font.pixelSize: Pe.small
                        color: Pe.inkMute
                    }
                    Rectangle {
                        x: 55
                        y: entry.topY + 18
                        width: 9; height: 9; radius: 4.5
                        color: entry.undone ? Pe.inkFaint : (entry.op === "delete_path" ? Pe.alarm : Pe.her)
                        Rectangle {
                            anchors.centerIn: parent
                            width: 21; height: 21; radius: 10.5
                            color: Pe.alpha(parent.color, 0.16)
                            visible: !entry.undone
                        }
                    }
                    RowLayout {
                        x: 84
                        y: entry.topY + 6
                        width: parent.width - 84
                        spacing: 12
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                PeIcon {
                                    name: page.icon(entry.op)
                                    size: 15
                                    color: entry.undone ? Pe.inkFaint : entry.op === "delete_path" ? Pe.alarm : Pe.inkSoft
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: entry.summary !== "" ? entry.summary : page.verb(entry.op)
                                    font.family: Pe.sans; font.pixelSize: 14; font.weight: Font.Medium
                                    color: entry.undone ? Pe.inkMute : Pe.ink
                                    font.strikeout: entry.undone
                                }
                            }
                            Text {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                Layout.leftMargin: 23
                                elide: Text.ElideMiddle
                                text: entry.path
                                font.family: Pe.mono; font.pixelSize: Pe.label
                                color: Pe.inkMute
                            }
                        }
                        Rectangle {
                            visible: entry.undone
                            implicitWidth: undoneText.implicitWidth + 18
                            implicitHeight: 24
                            radius: 12
                            color: Pe.alpha(Pe.ink, 0.06)
                            Text {
                                id: undoneText
                                textFormat: Text.PlainText
                                anchors.centerIn: parent
                                text: qsTr("undone")
                                font.family: Pe.sans; font.pixelSize: Pe.small; color: Pe.inkSoft
                            }
                        }
                        PeButton {
                            visible: !entry.undone
                            compact: true
                            kind: "secondary"
                            iconName: "undo"
                            text: qsTr("Undo")
                            tip: qsTr("Put back what was there before")
                            accessibleName: qsTr("Undo: %1").arg((entry.summary !== "" ? entry.summary : page.verb(entry.op))
                                                                 + (entry.path !== "" ? " " + entry.path : ""))
                            onClicked: {
                                // a failure is whispered; the conversation is not marked as failed
                                const why = page.stage.undo(entry.eid)
                                if (why === "")
                                    page.stage.whisper(qsTr("Undone: %1").arg(entry.summary !== "" ? entry.summary : page.verb(entry.op)), "success")
                                page.reload()
                            }
                        }
                    }
                }
            }
        }
    }
}
