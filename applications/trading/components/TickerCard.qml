// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// One market of the watchlist: a badge, the pair, the last price, the
// 24-hour change and the 24-hour line. Click to put it on the chart.
GlowCard {
    id: root

    required property var market   // {symbol, name, hasPrice, listed, price, changePct, spark, selected}
    signal chosen(string symbol)
    // Whether it can be taken off the list (not the last one).
    property bool removable: false
    signal removed(string symbol)

    readonly property bool up: market.changePct >= 0
    readonly property string base: Format.base(market.name)
    // A colour and a short mark per asset; these are letters, not logos.
    readonly property color badge: base === "BTC" ? Theme.accentAmber : base === "ETH" ? Theme.accentBlue
        : base === "SOL" ? Theme.accentEmerald : base === "PAXG" ? Theme.warning
        : base === "EUR" ? Theme.accentCyan : Theme.accent
    readonly property string mark: base === "PAXG" ? "Au" : base === "EUR" ? "€" : base.substring(0, 1)

    padding: Theme.spaceSm + 2
    glow: market.selected
    implicitWidth: 180
    implicitHeight: 64
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: market.hasPrice
        ? qsTr("%1, %2, %3 in 24 hours").arg(market.name).arg(Format.price(market.price)).arg(Format.signedPct(market.changePct))
        : market.listed === false ? qsTr("%1, not listed by the data service").arg(market.name)
        : qsTr("%1, no price yet").arg(market.name)
    Keys.onReturnPressed: chosen(market.symbol)
    Keys.onSpacePressed: chosen(market.symbol)
    Keys.onDeletePressed: if (removable) removed(market.symbol)

    Rectangle {
        anchors.fill: parent
        anchors.margins: -root.padding + 2
        radius: root.radius
        color: hover.hovered && !root.market.selected ? Theme.hoverOverlay : "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.accent
    }
    HoverHandler { id: hover }
    TapHandler {
        // A tap on the remove button is not a tap on the card.
        onTapped: (point) => {
            var at = remove.mapFromItem(root, point.position.x, point.position.y)
            if (!(remove.visible && remove.contains(at)))
                root.chosen(root.market.symbol)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm

        Rectangle {
            Layout.preferredWidth: 36
            Layout.preferredHeight: 36
            radius: width / 2
            color: root.badge
            Label {
                anchors.centerIn: parent
                text: root.mark
                color: Theme.bgBase
                font.pixelSize: root.mark.length > 1 ? Theme.fontSizeSmall : Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                Layout.fillWidth: true
                text: root.market.name
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
                elide: Text.ElideRight
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs + 2
                Label {
                    text: root.market.hasPrice ? Format.price(root.market.price)
                        : root.market.listed === false ? qsTr("Not listed") : "—"
                    color: root.market.listed === false ? Theme.textSecondary : Theme.textPrimary
                    font.pixelSize: root.market.listed === false ? Theme.fontSizeSmall : Theme.fontSizeBody
                    font.weight: Theme.weightSemibold
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.market.hasPrice
                    text: Format.signedPct(root.market.changePct)
                    color: root.up ? Theme.success : Theme.danger
                    font.pixelSize: Theme.fontSizeCaption
                    fontSizeMode: Text.HorizontalFit
                    minimumPixelSize: 8
                }
            }
        }
        NxSparkline {
            Layout.preferredWidth: 46
            Layout.fillHeight: true
            Layout.topMargin: Theme.spaceXs
            Layout.bottomMargin: Theme.spaceXs
            visible: root.market.spark.length > 1 && root.width > 210
            // Drawn from its lowest point so the day's shape shows.
            readonly property real floor: root.market.spark.length > 0 ? Math.min.apply(null, root.market.spark) : 0
            values: root.market.spark.map(v => v - floor)
            color: root.up ? Theme.success : Theme.danger
            fill: false
            gridLines: 0
            Accessible.ignored: true
        }
    }

    // Take the market off the list: shown while the pointer is on the card
    // (or the card has the keyboard focus; Delete does the same).
    Rectangle {
        id: remove
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: -root.padding + 3
        anchors.topMargin: -root.padding + 3
        width: 22
        height: 22
        radius: width / 2
        visible: root.removable && (hover.hovered || root.activeFocus)
        color: removeHover.hovered ? Theme.bgPressed : Theme.bgRaised
        border.width: 1
        border.color: Theme.borderStrong
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Remove %1 from the markets").arg(root.market.name)
        Accessible.onPressAction: root.removed(root.market.symbol)
        NxIcon {
            anchors.centerIn: parent
            name: "close"
            size: 16
            color: removeHover.hovered ? Theme.danger : Theme.textPrimary
        }
        HoverHandler { id: removeHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: root.removed(root.market.symbol) }
        ToolTip.visible: removeHover.hovered
        ToolTip.delay: 500
        ToolTip.text: qsTr("Remove %1 from the markets").arg(root.market.name)
    }
}
