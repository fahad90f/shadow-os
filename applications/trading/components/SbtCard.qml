// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// SBT Model 1 on the selected market: the setup in play (or the structure
// being watched), its levels, the retest, the PD arrays that confirm it, and
// the AI team's review. Bullish setups are drawn in the "up" colour, bearish
// ones in the "down" colour. Everything here is trading.sbt, from the candles.
GlowCard {
    id: root

    readonly property var sbt: trading.sbt
    readonly property var s: sbt.current || ({})
    readonly property bool has: s.id !== undefined
    readonly property bool bull: s.direction === "bullish"
    readonly property color sideTone: !has ? Theme.accent : bull ? Theme.success : Theme.danger

    function statusText(x) {
        switch (x.status) {
        case "armed": return x.intrabar ? qsTr("ARMED · price in the zone now (counts when the candle closes)")
                                        : x.level3Confirmed ? qsTr("ARMED · waiting for the retest of the Level 2 zone")
                                                            : qsTr("ARMED · Level 3 still forming")
        case "triggered": return qsTr("RETEST CONFIRMED at %1 · entry at the next candle's open").arg(Format.clock(x.retest.time))
        case "entered": return x.pending ? qsTr("ENTRY · waiting for the AI team's review")
                                         : x.traded ? qsTr("ENTERED · on the paper account") : qsTr("ENTRY SIGNAL")
        case "target": return qsTr("LEVEL 3 REACHED")
        case "stopped": return qsTr("STOPPED")
        case "invalidated": return qsTr("INVALIDATED · %1").arg(x.note)
        case "skipped": return qsTr("NOT TAKEN · %1").arg(x.note)
        case "expired": return qsTr("EXPIRED · %1").arg(x.note)
        }
        return ""
    }
    function pdLabel(kind) {
        return kind === "fvg" ? qsTr("FVG") : kind === "order_block" ? qsTr("Order block")
             : kind === "demand" ? qsTr("Demand") : kind === "supply" ? qsTr("Supply") : kind
    }

    implicitHeight: body.implicitHeight + padding * 2
    padding: Theme.spaceSm + 2
    tone: sideTone
    glow: has && (s.status === "triggered" || s.status === "entered")
    Accessible.role: Accessible.Grouping
    Accessible.name: has ? qsTr("SBT Model 1, %1 setup: %2").arg(s.direction).arg(statusText(s))
                         : qsTr("SBT Model 1: no setup")

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon { name: "target"; size: Theme.iconSmall + 2; color: root.sideTone }
            Label {
                text: qsTr("SBT MODEL 1")
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightSemibold
                font.letterSpacing: 1.2
            }
            NxBadge {
                visible: root.has
                text: root.bull ? qsTr("BULLISH") : qsTr("BEARISH")
                kind: root.bull ? "success" : "danger"
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: root.has && root.s.review !== undefined
                text: !root.has ? ""
                    : root.s.review.running ? qsTr("AI team reviewing…")
                    : root.s.review.done ? (root.s.review.confirmed ? qsTr("AI team: confirmed %1 of %2").arg(root.s.review.agreeing).arg(root.s.review.voters)
                                                                    : qsTr("AI team: not confirmed"))
                    : root.sbt.aiConfirm && root.sbt.aiAvailable ? qsTr("AI review pending")
                    : qsTr("Rules alone")
                color: root.has && root.s.review.done && !root.s.review.confirmed ? Theme.warning : Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
            }
        }
        Label {
            Layout.fillWidth: true
            text: root.has ? root.statusText(root.s)
                : root.sbt.watching && (root.sbt.watching.bullish.price || root.sbt.watching.bearish.price)
                    ? qsTr("Watching for a break of structure: above %1 (bullish) / below %2 (bearish)")
                          .arg(Format.price(root.sbt.watching.bullish.price)).arg(Format.price(root.sbt.watching.bearish.price))
                    : qsTr("Watching for a 1-2-3 with a break of structure.")
            color: root.has ? root.sideTone : Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Theme.weightSemibold
            wrapMode: Text.Wrap
        }
        // Entry, stop and target are on the cards above; here the structure.
        GridLayout {
            Layout.fillWidth: true
            visible: root.has
            columns: 4
            columnSpacing: Theme.spaceSm
            rowSpacing: 2
            component Cell: ColumnLayout {
                property string label
                property string value
                property color valueColor: Theme.textPrimary
                spacing: 0
                Layout.fillWidth: true
                Label { text: parent.label; color: Theme.textMuted; font.pixelSize: Theme.fontSizeCaption }
                Label { text: parent.value; color: parent.valueColor; font.pixelSize: Theme.fontSizeSmall; font.family: Theme.fontMono }
            }
            Cell { label: qsTr("Level 1 · BOS"); value: root.has ? Format.price(root.s.level1.price) : "" }
            Cell { label: qsTr("Level 2"); value: root.has ? Format.price(root.s.level2.price) : "" }
            Cell { label: qsTr("Zone"); value: root.has ? Format.price(root.s.zone.bottom) + "–" + Format.price(root.s.zone.top) : "" }
            Cell { label: root.has && root.s.level3Confirmed ? qsTr("Level 3 · target") : qsTr("Level 3 (forming)")
                   value: root.has ? Format.price(root.s.target) : ""; valueColor: root.sideTone }
        }
        Flow {
            Layout.fillWidth: true
            visible: root.has && root.s.confirmations.length > 0
            spacing: 4
            Label { text: qsTr("PD arrays (extra):"); color: Theme.textMuted; font.pixelSize: Theme.fontSizeCaption; height: 20; verticalAlignment: Text.AlignVCenter }
            Repeater {
                model: root.has ? root.s.confirmations : []
                NxBadge {
                    required property var modelData
                    text: root.pdLabel(modelData.kind) + " " + Format.price(modelData.bottom) + "–" + Format.price(modelData.top)
                    kind: "accent"
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.has && (root.s.outcome || "") !== "" && !root.s.traded
            text: root.has ? root.s.outcome : ""
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
        }
    }
}
