// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// Toasts as whispers: short lines in the top line, near where Shadow sits,
// that fade after a few seconds (tap to dismiss). One at a time, so a
// whisper never grows down over the dialogue: a newer one waits until the
// current one has been readable for a moment (or replaces it at once while
// screenshots freeze time).
// API kept from NxToaster: show(text, kind) with kind info|success|warning|danger.
Item {
    id: toaster
    property real bandHeight: 64          // the top line the whisper is centred in
    property int duration: 3800
    property int minShow: 1500            // how long one shows at least when others wait
    property bool sticky: false
    property real maxWidth: 560
    // the horizontal lane the whisper may use (centred on the window when it fits)
    property real laneLeft: 16
    property real laneRight: width - 16
    readonly property real fitWidth: Math.max(160, Math.min(maxWidth, laneRight - laneLeft))
    readonly property int count: queue.count
    z: 1000

    // what is on screen now
    property string text: ""
    property string kind: "info"
    property double shownAt: 0

    function show(text: string, kind: string) {
        const t = String(text)
        // the same message twice: once is enough
        for (let i = 0; i < queue.count; ++i)
            if (queue.get(i).text === t)
                return
        if (sticky && queue.count > 0)
            queue.clear()           // frozen time: the newest is the one to see
        queue.append({ text: t, kind: kind || "info" })
        while (queue.count > 4)
            queue.remove(1)         // the oldest waiting ones go first
        if (queue.count === 1)
            present()
        else
            schedule()
    }
    // Takes a whisper away (e.g. "Shadow is listening…" once she has heard you).
    function dismiss(text: string) {
        for (let i = queue.count - 1; i >= 0; --i) {
            if (queue.get(i).text === String(text)) {
                if (i === 0)
                    advance()
                else
                    queue.remove(i)
                return
            }
        }
    }
    function present() {
        if (queue.count === 0) {
            toaster.text = ""
            life.stop()
            return
        }
        toaster.text = queue.get(0).text
        toaster.kind = queue.get(0).kind
        shownAt = Date.now()
        fadeIn.restart()
        schedule()
    }
    function advance() {
        if (queue.count > 0)
            queue.remove(0)
        present()
    }
    function schedule() {
        if (sticky || queue.count === 0) {
            life.stop()
            return
        }
        const shown = Date.now() - shownAt
        life.interval = Math.max(1, (queue.count > 1 ? minShow : duration) - shown)
        life.restart()
    }
    ListModel { id: queue }
    Timer {
        id: life
        onTriggered: toaster.advance()
    }

    Rectangle {
        id: bubble
        readonly property color tone: toaster.kind === "success" ? Pe.ok : toaster.kind === "warning" ? Pe.caution
                                    : toaster.kind === "danger" ? Pe.alarm : Pe.her
        visible: queue.count > 0 && toaster.text !== ""
        width: Math.min(toaster.fitWidth, label.implicitWidth + 62)
        height: Math.max(40, label.implicitHeight + 20)
        x: Math.round(Math.max(toaster.laneLeft, Math.min((toaster.width - width) / 2, toaster.laneRight - width)))
        y: Math.round(Math.max(4, (toaster.bandHeight - height) / 2))
        radius: height > 44 ? 14 : height / 2
        color: Pe.dark ? "#121827" : "#FFFFFF"
        border.width: 1
        border.color: Pe.alpha(tone, 0.45)
        Accessible.role: Accessible.Notification
        Accessible.name: toaster.text
        NumberAnimation on opacity {
            id: fadeIn
            from: 0; to: 1
            duration: Pe.tQuick * 1.6
            running: false
        }
        Rectangle {
            x: 16
            anchors.verticalCenter: parent.verticalCenter
            width: 8; height: 8; radius: 4
            color: bubble.tone
            Rectangle { anchors.centerIn: parent; width: 16; height: 16; radius: 8; color: Pe.alpha(bubble.tone, 0.18) }
        }
        Text {
            id: label
            textFormat: Text.PlainText
            x: 36
            width: Math.min(implicitWidth, toaster.fitWidth - 54)
            anchors.verticalCenter: parent.verticalCenter
            text: toaster.text
            wrapMode: Text.Wrap
            maximumLineCount: 3
            elide: Text.ElideRight
            font.family: Pe.sans
            font.pixelSize: Pe.ui
            color: Pe.ink
        }
        TapHandler {
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: toaster.advance()
        }
    }
}
