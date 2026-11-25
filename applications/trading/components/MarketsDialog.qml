// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// Add a market to the list: search what the data service lists, add it with
// one click. Opening it fetches the list.
Dialog {
    id: root

    readonly property int count: trading.watchlist.length
    readonly property bool full: count >= trading.maxMarkets
    // The matches; evaluated again when the list arrives or a market is added.
    readonly property var matches: {
        void trading.marketsLoaded
        void trading.watchlist
        return trading.findMarkets(search.text)
    }

    title: qsTr("Add a market")
    modal: true
    width: Math.min(480, parent ? parent.width - Theme.spaceXxl : 480)
    height: Math.min(560, parent ? parent.height - Theme.spaceXxl : 560)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    footer: null

    onOpened: {
        search.text = ""
        search.forceActiveFocus()
        trading.refreshMarkets()
    }

    function add(symbol) {
        var error = trading.addMarket(symbol)
        if (error !== "")
            ApplicationWindow.window.toaster.show(error, "danger")
        else
            trading.symbol = symbol   // onto the chart at once
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        NxSearchField {
            id: search
            Layout.fillWidth: true
            placeholderText: trading.marketKind === "forex" ? qsTr("Search: EUR, GBP, CHF, gold (PAXG)…")
                                                              : qsTr("Search: BTC, DOGE, XRP, EUR…")
            // Enter adds the first match.
            onAccepted: if (!root.full && root.matches.length > 0 && !root.matches[0].listed) root.add(root.matches[0].symbol)
        }
        Label {
            Layout.fillWidth: true
            text: root.full
                ? qsTr("The list is full (%1 markets). Remove one to add another.").arg(trading.maxMarkets)
                : trading.marketKind === "forex"
                    ? qsTr("%1 of %2 markets in the list. Offered: currency pairs with the US dollar, and gold, that trade often enough at %3 for their prices to mean something.").arg(root.count).arg(trading.maxMarkets).arg(trading.dataSource)
                    : qsTr("%1 of %2 markets in the list. Prices come from %3.").arg(root.count).arg(trading.maxMarkets).arg(trading.dataSource)
            color: root.full ? Theme.warning : Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.WordWrap
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.matches
            spacing: 2
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Markets")

            delegate: Rectangle {
                id: row
                required property var modelData
                width: ListView.view.width - Theme.spaceSm
                height: 44
                radius: Theme.radiusSm
                color: rowHover.hovered ? Theme.hoverOverlay : "transparent"
                HoverHandler { id: rowHover }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceSm
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceSm
                    Label {
                        Layout.fillWidth: true
                        text: row.modelData.name
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightMedium
                        elide: Text.ElideRight
                    }
                    Label {
                        text: Format.price(row.modelData.price)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    NxBadge {
                        visible: row.modelData.listed
                        kind: "success"
                        text: qsTr("In the list")
                    }
                    NxButton {
                        visible: !row.modelData.listed
                        enabled: !root.full
                        iconName: "plus"
                        text: qsTr("Add")
                        Accessible.name: qsTr("Add %1").arg(row.modelData.name)
                        onClicked: root.add(row.modelData.symbol)
                    }
                }
            }

            NxEmptyState {
                anchors.centerIn: parent
                width: parent.width - Theme.spaceXl
                visible: list.count === 0
                icon: trading.marketsError !== "" ? "alert-triangle" : "search"
                tone: trading.marketsError !== "" ? "warning" : "neutral"
                title: trading.marketsLoading ? qsTr("Fetching the markets…")
                    : trading.marketsError !== "" ? qsTr("The list of markets could not be fetched")
                    : qsTr("No market matches “%1”").arg(search.text)
                message: trading.marketsLoading ? "" : trading.marketsError !== "" ? trading.marketsError
                    : qsTr("Search by the short name of the asset, for example BTC or EUR.")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxButton {
                visible: trading.marketsError !== ""
                iconName: "refresh"
                text: qsTr("Try again")
                onClicked: trading.refreshMarkets()
            }
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Done")
                onClicked: root.close()
            }
        }
    }
}
