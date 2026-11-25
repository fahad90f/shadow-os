// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// The right-hand column of the desk: what the analysts say together, the
// levels of the trade, why, and who took part.
ColumnLayout {
    id: root

    property Labels labels
    signal openPage(string key)

    readonly property var signalData: trading.signal
    readonly property string state: signalData.state || "none"
    readonly property bool ready: state === "ready"
    readonly property string action: ready ? signalData.action : "hold"
    readonly property bool directional: ready && action !== "hold"
    readonly property var readings: trading.readings
    // The open position on the selected market. While there is one, the
    // levels shown are its levels, not the newest signal's.
    readonly property var position: trading.position
    readonly property bool holding: position.id !== undefined
    readonly property color actionTone: labels.actionColor(action)
    // How many of the things on the panel are not in working order.
    readonly property int issues: {
        var n = 0
        var all = trading.analysts
        for (var i = 0; i < all.length; i++) {
            if (all[i].kind === "model")
                continue
            if (["error", "no_key", "halted", "waiting"].indexOf(all[i].state) >= 0)
                n++
        }
        return n
    }

    // One line on the strength of the agreement.
    function headline() {
        if (!ready)
            return readings.taValid === true ? qsTr("No analysis yet") : qsTr("Waiting for price history")
        if (!directional)
            return signalData.note
        var side = action === "buy" ? qsTr("bullish") : qsTr("bearish")
        var strength = signalData.confidence >= 75 ? qsTr("Strong %1 agreement").arg(side)
            : signalData.confidence >= 60 ? qsTr("Clear %1 agreement").arg(side)
            : qsTr("Weak %1 agreement").arg(side)
        return qsTr("%1 · %2 of %3 analysts").arg(strength).arg(signalData.agreeing).arg(signalData.voters)
    }

    spacing: Theme.spaceSm

    // ---- the analysts' panel ----------------------------------------------------
    GlowCard {
        Layout.fillWidth: true
        Layout.fillHeight: true
        glow: true
        implicitHeight: 400

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceSm

            // Title, status, and the art.
            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 66
                Layout.maximumHeight: 66
                spacing: Theme.spaceSm
                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: Theme.radiusMd
                    color: Theme.alpha(Theme.accent, 0.16)
                    border.width: Theme.borderWidth
                    border.color: Theme.alpha(Theme.accent, 0.6)
                    NxIcon { anchors.centerIn: parent; name: "radar"; size: Theme.icon; color: Theme.accent }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Label {
                        text: qsTr("Shadow AI Intelligence")
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeHeading
                        font.weight: Theme.weightSemibold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: trading.analyzing ? (root.signalData.stage === "review"
                                ? qsTr("Team review…  •  The analysis team compares its calls…")
                                : qsTr("Analyzing market…  •  Finding best opportunities…"))
                            : root.ready ? qsTr("%1 %2  •  analyzed %3").arg(root.signalData.symbolName)
                                  .arg(root.signalData.interval).arg(Format.clock(root.signalData.time))
                            : root.readings.taValid === true ? qsTr("Ready  •  runs when the next candle closes")
                            : qsTr("Waiting for price history…")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideRight
                    }
                }
                NeuralArt {
                    Layout.preferredWidth: 112
                    Layout.preferredHeight: 66
                    busy: trading.analyzing
                }
            }

            // The call, the confidence, and the auto trade switch.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                GlowCard {
                    Layout.fillWidth: true
                    implicitHeight: 96
                    padding: Theme.spaceSm + 2
                    tone: root.directional ? root.actionTone : Theme.accent
                    Accessible.name: qsTr("Signal: %1. %2. Confidence %3.").arg(root.labels.action(root.action))
                        .arg(root.headline()).arg(root.ready ? Format.number(root.signalData.confidence, 1) : "—")

                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceSm
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            RowLayout {
                                spacing: Theme.spaceSm
                                NxIcon {
                                    visible: root.directional
                                    name: root.action === "buy" ? "arrow-up" : "arrow-down"
                                    size: Theme.iconLarge
                                    color: root.actionTone
                                }
                                Label {
                                    text: root.ready ? root.labels.action(root.action) : "—"
                                    color: root.ready ? root.actionTone : Theme.textMuted
                                    font.pixelSize: Theme.fontSizeDisplay + 6
                                    font.weight: Theme.weightSemibold
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                text: root.headline()
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }
                        NxGauge {
                            id: gauge
                            Layout.preferredWidth: 84
                            Layout.preferredHeight: 84
                            // Empty until there is a signal; the text inside is drawn below.
                            value: root.ready ? root.signalData.confidence / 100 : 0
                            color: root.directional ? root.actionTone : Theme.accent
                            HoverHandler { id: gaugeHover }
                            ToolTip.visible: gaugeHover.hovered
                            ToolTip.text: qsTr("How strongly and how unanimously the analysts agree (0 to 100).\nIt is not a probability of profit.")
                            Column {
                                anchors.centerIn: parent
                                spacing: 0
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: qsTr("Confidence")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSizeCaption - 1
                                }
                                Label {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: root.ready ? Format.number(root.signalData.confidence, 1) + "%" : "—"
                                    color: root.directional ? root.actionTone : root.ready ? Theme.textPrimary : Theme.textMuted
                                    font.pixelSize: Theme.fontSizeBodyLarge
                                    font.weight: Theme.weightSemibold
                                }
                            }
                        }
                    }
                }

                // Auto trade: a button that shows its state.
                Rectangle {
                    id: auto
                    readonly property bool on: trading.autoTrade
                    Layout.preferredWidth: 118
                    Layout.preferredHeight: 96
                    radius: Theme.radiusLg
                    border.width: activeFocus ? 2 : Theme.borderWidth
                    border.color: activeFocus ? Theme.textPrimary : Theme.alpha(Theme.accent, on ? 1 : 0.45)
                    gradient: Gradient {
                        GradientStop { position: 0; color: Theme.alpha(Theme.accent, auto.on ? 0.55 : 0.10) }
                        GradientStop { position: 1; color: Theme.alpha(Theme.accent, auto.on ? 0.25 : 0.04) }
                    }
                    activeFocusOnTab: true
                    Accessible.role: Accessible.CheckBox
                    Accessible.name: qsTr("Auto trade")
                    Accessible.checked: on
                    Accessible.description: qsTr("Place signals that pass the risk rules on the paper account without asking")
                    Keys.onReturnPressed: trading.autoTrade = !on
                    Keys.onSpacePressed: trading.autoTrade = !on
                    HoverHandler { id: autoHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: trading.autoTrade = !auto.on }
                    ToolTip.visible: autoHover.hovered
                    ToolTip.delay: 400
                    ToolTip.text: auto.on ? qsTr("On: signals that pass the risk rules are placed on the paper account. Click to turn off.")
                                          : qsTr("Off: signals are shown, nothing is placed. Click to turn on.")

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 2
                        NxIcon {
                            Layout.alignment: Qt.AlignHCenter
                            name: "zap"
                            size: Theme.iconLarge
                            color: auto.on ? Theme.textPrimary : Theme.accent
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Auto Trade")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightSemibold
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: auto.on ? qsTr("(Enabled)") : qsTr("(Disabled)")
                            color: auto.on ? Theme.success : Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }
            }

            // Entry, stop, target.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Repeater {
                    model: [
                        { label: qsTr("Entry Price"), tone: Theme.success,
                          value: root.holding ? root.position.entry : root.directional ? root.signalData.entry : 0 },
                        { label: qsTr("Stop Loss"), tone: Theme.warning,
                          value: root.holding ? root.position.stop : root.directional ? root.signalData.stop : 0 },
                        { label: qsTr("Take Profit"), tone: Theme.success,
                          value: root.holding ? root.position.takeProfit : root.directional ? root.signalData.takeProfit : 0 },
                    ]
                    GlowCard {
                        required property var modelData
                        Layout.fillWidth: true
                        padding: Theme.spaceSm + 2
                        implicitWidth: 100
                        implicitHeight: 58
                        Accessible.name: modelData.label + ": " + Format.price(modelData.value)
                        RowLayout {
                            anchors.fill: parent
                            spacing: Theme.spaceSm
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                width: 12; height: 12; radius: 6
                                color: modelData.tone
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    text: modelData.label
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSizeCaption
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: Format.price(modelData.value)
                                    color: Theme.textPrimary
                                    font.pixelSize: Theme.fontSizeBodyLarge
                                    font.weight: Theme.weightSemibold
                                    fontSizeMode: Text.HorizontalFit
                                    minimumPixelSize: 10
                                }
                            }
                        }
                    }
                }
            }

            // The open position: there until it closes.
            NxBanner {
                Layout.fillWidth: true
                visible: root.holding
                kind: root.holding && root.position.pnl < 0 ? "warning" : "success"
                title: root.holding ? qsTr("Open %1 · %2 %3").arg(root.labels.action(root.position.side))
                    .arg(Format.quantity(root.position.quantity)).arg(Format.base(root.position.name)) : ""
                message: root.holding ? qsTr("Profit %1 %2 (%3) at %4. The entry, stop and target above are this position's.")
                    .arg(Format.signedMoney(root.position.pnl)).arg(trading.accountCurrency)
                    .arg(Format.signedPct(root.position.pnlPct)).arg(Format.price(root.position.price)) : ""
                actionText: qsTr("Close position")
                onActionTriggered: trading.closePosition(root.position.id)
            }

            // What happened with the signal.
            NxBanner {
                Layout.fillWidth: true
                visible: root.ready && (root.signalData.outcome || "") !== "" && !(root.holding && root.signalData.traded)
                kind: root.signalData.traded ? "success" : root.signalData.placeable ? "info" : "warning"
                message: (root.signalData.outcome || "")
                    + (root.directional && root.signalData.risk.quantity > 0
                        ? "  " + qsTr("Size %1 %2, %3 %4 at risk, reward to risk %5.")
                              .arg(Format.quantity(root.signalData.risk.quantity)).arg(Format.base(root.signalData.symbolName))
                              .arg(Format.money(root.signalData.risk.riskAmount)).arg(trading.accountCurrency)
                              .arg(Format.number(root.signalData.rewardRisk, 2))
                        : "")
                actionText: root.signalData.placeable && !trading.autoTrade ? qsTr("Place paper trade") : ""
                onActionTriggered: {
                    var error = trading.placeSuggestedTrade()
                    if (error !== "")
                        ApplicationWindow.window.toaster.show(error, "danger")
                }
            }

            // Why.
            GlowCard {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: 86
                implicitHeight: 110
                padding: Theme.spaceSm + 2

                ColumnLayout {
                    anchors.fill: parent
                    spacing: Theme.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        NxIcon { name: "star"; size: Theme.iconSmall; color: Theme.accent }
                        Label {
                            Layout.fillWidth: true
                            // Whose reasoning it is: the most confident analyst on the winning side.
                            text: root.ready && (root.signalData.leadName || "") !== ""
                                ? qsTr("AI Reasoning · %1").arg(root.signalData.leadName) : qsTr("AI Reasoning")
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightSemibold
                            elide: Text.ElideRight
                        }
                        Button {
                            text: qsTr("Analyze now")
                            flat: true
                            icon.source: "image://nxicon/refresh"
                            enabled: !trading.analyzing && root.readings.taValid === true
                            onClicked: trading.analyzeNow()
                            ToolTip.visible: hovered && root.readings.taValid !== true
                            ToolTip.text: qsTr("Needs 60 closed candles of price history.")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: Theme.spaceSm
                        ScrollView {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            contentWidth: availableWidth
                            Label {
                                width: parent.width
                                text: !root.ready ? qsTr("The analysts' reasoning appears here after the first analysis.")
                                    : (root.signalData.reasoning || "") !== "" ? root.signalData.reasoning
                                    : root.signalData.note
                                color: root.ready ? Theme.textSecondary : Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                wrapMode: Text.WordWrap
                            }
                        }
                        ColumnLayout {
                            Layout.preferredWidth: 112
                            Layout.fillHeight: true
                            spacing: 0
                            // The last candles' closes, drawn from their lowest point.
                            NxSparkline {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                readonly property var series: trading.closes
                                readonly property real floor: series.length > 0 ? Math.min.apply(null, series) : 0
                                values: series.map(v => v - floor)
                                color: Theme.accentBlue
                                gridLines: 0
                                Accessible.ignored: true
                            }
                            LinkButton {
                                Layout.alignment: Qt.AlignRight
                                text: qsTr("View Details")
                                onClicked: root.openPage("analysis")
                            }
                        }
                    }
                }
            }
        }
    }

    // ---- who takes part -------------------------------------------------------
    Panel {
        Layout.fillWidth: true
        implicitHeight: 128
        title: qsTr("Models & Skills in Action")
        icon: "star"
        trailing: [
            StatusDot {
                text: root.issues === 0 ? qsTr("All Systems Online")
                    : root.issues === 1 ? qsTr("1 needs attention") : qsTr("%1 need attention").arg(root.issues)
                tone: root.issues === 0 ? Theme.success : Theme.warning
            },
            LinkButton { text: qsTr("Panel"); onClicked: root.openPage("models") }
        ]

        // Four roles of the model panel, then the four skills.
        RowLayout {
            anchors.fill: parent
            spacing: Theme.spaceXs
            Repeater {
                model: trading.analysts.filter(a => a.kind !== "model")
                AnalystTile {
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    analyst: modelData
                    labels: root.labels
                }
            }
        }
    }
}
