// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// First meeting: no API key yet. Her face is still scattered starlight
// (mood "dormant"); she explains, in her own words, the two ways to give her
// a key, and wakes up on Reload. The key itself is never shown here.
Item {
    id: page
    required property StageModel stage
    readonly property bool wide: width >= 980
    readonly property real faceSize: wide ? Math.min(300, height * 0.42) : Math.min(150, height * 0.26)
    readonly property rect faceRect: wide
        ? Qt.rect(Math.max(40, (width - 1040) / 2 + 40), Math.max(90, (height - faceSize) / 2 - 30), faceSize, faceSize)
        : Qt.rect((width - faceSize) / 2, flick.y - flick.contentY, faceSize, faceSize)
    // Small windows: she scrolls away with the page and fades out before
    // she would reach the top line (the page itself is clipped below it).
    readonly property real faceOpacity: wide ? 1 : Math.max(0, Math.min(1, 1 - flick.contentY / (faceSize * 0.45)))
    readonly property AssistantConfig cfg: stage.cfg
    readonly property string envLine: "export SHADOW_ASSISTANT_API_KEY=sk-ant-…"
    readonly property string confLines: "[api]\nkey=sk-ant-…"

    function wake() {
        stage.reload()
        if (cfg.apiKeyConfigured)
            stage.whisper(qsTr("API key found — you're ready to go"), "success")
        else
            stage.whisper(qsTr("Still no key found at the expected location"), "warning")
    }

    Text {
        textFormat: Text.PlainText
        visible: page.wide
        x: page.faceRect.x
        width: page.faceRect.width
        y: page.faceRect.y + page.faceRect.height + 4
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("asleep — waiting for a key")
        font.family: Pe.sans; font.pixelSize: Pe.small; font.italic: true
        color: Pe.inkMute
    }

    Flickable {
        id: flick
        x: page.wide ? page.faceRect.x + page.faceRect.width + 56 : 16
        // below the top line (the wordmark and the gear are never scrolled under)
        y: 64
        width: page.wide ? Math.min(600, parent.width - x - 40) : parent.width - 32
        height: parent.height - y
        contentHeight: col.y + col.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: PeScrollBar {}

        ColumnLayout {
            id: col
            width: flick.width
            y: page.wide ? Math.max(0, (flick.height - implicitHeight) / 2 - 10) : page.faceSize + 12
            spacing: 14

            Text {
                textFormat: Text.PlainText
                text: qsTr("BEFORE WE MEET")
                font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Bold
                font.letterSpacing: Pe.labelTracking
                color: Pe.her
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("I need a key to wake up.")
                font.family: Pe.sans; font.pixelSize: page.wide ? 34 : 28; font.weight: Font.Light
                color: Pe.ink
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                lineHeight: 1.4
                text: qsTr("I think with Anthropic's Claude. Give me an API key in one of the two ways below — it stays on this computer, and this app never stores it.")
                font.family: Pe.sans; font.pixelSize: Pe.body
                color: Pe.inkSoft
            }

            Step {
                Layout.topMargin: 10
                stage: page.stage
                number: "1"
                title: qsTr("Set an environment variable")
                note: qsTr("SHADOW_ASSISTANT_API_KEY (or ANTHROPIC_API_KEY): add it to your shell profile, then start me again.")
                code: page.envLine
                lang: qsTr("shell")
            }
            Step {
                stage: page.stage
                number: "2"
                title: qsTr("…or put it in my configuration file")
                note: qsTr("%1 — create it with owner-only permissions (chmod 600).").arg(page.cfg.configPath)
                code: page.confLines
                lang: qsTr("assistant.conf")
                PeButton {
                    compact: true; kind: "ghost"; iconName: "copy"; text: qsTr("Copy path")
                    onClicked: {
                        page.stage.agent.copyToClipboard(page.cfg.configPath)
                        page.stage.whisper(qsTr("Path copied to the clipboard"), "success")
                    }
                }
                PeButton {
                    compact: true; kind: "ghost"; iconName: "file-text"; text: qsTr("Copy full template")
                    onClicked: {
                        page.stage.agent.copyToClipboard(page.cfg.configTemplate)
                        page.stage.whisper(qsTr("Template copied to the clipboard"), "success")
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 8
                spacing: 14
                PeButton {
                    kind: "primary"
                    iconName: "refresh"
                    text: qsTr("Wake Shadow")
                    tip: qsTr("Reload the configuration")
                    onClicked: page.wake()
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Reloads the configuration — no restart needed for the file.")
                    font.family: Pe.sans; font.pixelSize: Pe.small
                    color: Pe.inkMute
                }
            }
            Rectangle { Layout.fillWidth: true; Layout.topMargin: 10; implicitHeight: 1; color: Pe.line }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Rather stay offline? I can run on a local model instead — nothing leaves this computer.")
                    font.family: Pe.sans; font.pixelSize: Pe.small
                    color: Pe.inkSoft
                }
                PeButton {
                    compact: true
                    kind: "secondary"
                    text: qsTr("Offline mode")
                    onClicked: page.stage.openSettings("claude")
                }
            }
        }
    }

    component Step: ColumnLayout {
        id: step
        required property StageModel stage
        property string number: ""
        property string title: ""
        property string note: ""
        property string code: ""
        property string lang: ""
        default property alias extra: extraRow.data
        Layout.fillWidth: true
        spacing: 8
        RowLayout {
            spacing: 10
            Rectangle {
                implicitWidth: 24; implicitHeight: 24; radius: 12
                color: "transparent"
                border.width: 1
                border.color: Pe.alpha(Pe.violet, 0.6)
                Text { textFormat: Text.PlainText; anchors.centerIn: parent; text: step.number; font.family: Pe.mono; font.pixelSize: Pe.small; color: Pe.her }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: step.title
                font.family: Pe.sans; font.pixelSize: 15; font.weight: Font.DemiBold
                color: Pe.ink
            }
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.leftMargin: 34
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            text: step.note
            font.family: Pe.sans; font.pixelSize: Pe.small
            color: Pe.inkMute
        }
        CodeSlab {
            Layout.fillWidth: true
            Layout.leftMargin: 34
            stage: step.stage
            code: step.code
            lang: step.lang
            plainLabel: true
        }
        Row {
            id: extraRow
            Layout.leftMargin: 28
            spacing: 4
            visible: children.length > 0
        }
    }
}
