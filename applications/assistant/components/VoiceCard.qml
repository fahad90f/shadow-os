// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import org.shadow.assistant

// One of her voices: name, character, a signature wave, and a preview.
Rectangle {
    id: card
    property var voiceInfo: ({})
    property bool selected: false
    signal chosen()
    signal preview()

    readonly property string vid: voiceInfo.id || ""
    readonly property bool available: voiceInfo.available !== false
    // a stable "signature" per voice, so each card has its own wave
    readonly property real seed: {
        let h = 0
        for (let i = 0; i < vid.length; ++i)
            h = (h * 31 + vid.charCodeAt(i)) % 997
        return h / 997
    }

    // the words on top, the signature wave and the preview button below them
    implicitHeight: Math.max(100, info.y + info.implicitHeight + 46)
    radius: 14
    color: selected ? (Pe.dark ? "#121527" : "#F6F3FF") : (hover.hovered ? Pe.surfaceHi : Pe.card)
    border.width: selected ? 1.5 : 1
    border.color: selected ? Pe.alpha(Pe.violet, 0.75) : Pe.line
    // a voice that needs the natural voice is said so in words (its text
    // stays readable; a faded card would not be)
    Accessible.role: Accessible.RadioButton
    Accessible.name: (voiceInfo.name || "") + ", " + (voiceInfo.description || "") + ", " + (voiceInfo.accent || "")
                     + (available ? "" : ", " + qsTr("needs the natural voice"))
    Accessible.checked: selected
    activeFocusOnTab: true
    Keys.onSpacePressed: card.chosen()
    Keys.onReturnPressed: card.chosen()

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: card.chosen() }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: 17
        color: "transparent"
        border.width: 2
        border.color: Pe.cyan
        visible: card.activeFocus
    }

    Column {
        id: info
        x: 16; y: 14
        width: parent.width - 32
        spacing: 2
        // name, accent, and the check of the chosen one
        Row {
            spacing: 8
            width: parent.width
            Text {
                id: nameText
                textFormat: Text.PlainText
                text: card.voiceInfo.name || ""
                font.family: Pe.sans
                font.pixelSize: 17
                font.weight: Font.DemiBold
                color: Pe.ink
            }
            Text {
                textFormat: Text.PlainText
                anchors.baseline: nameText.baseline
                width: Math.min(implicitWidth, parent.width - nameText.width - (check.visible ? 34 : 8))
                elide: Text.ElideRight
                text: card.voiceInfo.accent || ""
                font.family: Pe.sans
                font.pixelSize: Pe.small
                color: Pe.inkMute
            }
            Rectangle {
                id: check
                visible: card.selected
                anchors.verticalCenter: parent.verticalCenter
                width: 18; height: 18; radius: 9
                color: Pe.alpha(Pe.violet, 0.2)
                PeIcon { anchors.centerIn: parent; name: "check"; size: 12; color: Pe.her }
            }
        }
        // what she sounds like: up to two lines, the rest in a tooltip
        Text {
            id: descText
            textFormat: Text.PlainText
            width: parent.width
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            text: card.voiceInfo.description || ""
            font.family: Pe.sans
            font.pixelSize: Pe.small
            color: Pe.inkMute
        }
        Text {
            visible: !card.available
            textFormat: Text.PlainText
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("Needs the natural voice")
            font.family: Pe.sans
            font.pixelSize: Pe.small
            font.weight: Font.Medium
            color: Pe.chipText(Pe.caution)
        }
    }
    ToolTip.visible: hover.hovered && descText.truncated
    ToolTip.text: (voiceInfo.description || "") + " · " + (voiceInfo.accent || "")
    ToolTip.delay: 500

    // signature wave: drawn once in a fixed box and stretched to the card
    // (the path itself never changes, so no geometry is rebuilt on resize)
    Shape {
        x: 16
        y: parent.height - 36
        width: 200
        height: 22
        transform: Scale { xScale: Math.max(0, card.width - 70) / 200 }
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 1.6
            strokeColor: card.selected ? Pe.her : Pe.alpha(Pe.inkSoft, card.available ? 0.6 : 0.3)
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathPolyline {
                path: {
                    const pts = []
                    const n = 40
                    const f = 2.2 + card.seed * 3.5, f2 = 5 + card.seed * 4
                    for (let i = 0; i <= n; ++i) {
                        const t = i / n
                        const env = Math.sin(Math.PI * t)
                        const y = 11 + env * (7.5 * Math.sin(2 * Math.PI * f * t + card.seed * 6)
                                              + 2.5 * Math.sin(2 * Math.PI * f2 * t))
                        pts.push(Qt.point(t * 200, y))
                    }
                    return pts
                }
            }
        }
    }

    PeIconButton {
        anchors.right: parent.right
        anchors.rightMargin: 10
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        size: 34
        quiet: false
        enabled: card.available
        iconName: "play"
        iconSize: 14
        tint: Pe.ink
        tip: qsTr("Hear %1").arg(card.voiceInfo.name || "")
        onClicked: card.preview()
    }
}
