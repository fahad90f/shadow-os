// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.installer

StepPage {
    id: page
    required property WizardState wiz

    property string region: "Europe"
    property int tick: 0

    title: qsTr("Time zone")
    subtitle: qsTr("Choose the place whose time the installed system should show.")
    ready: TimeZones.problem !== "" || TimeZones.contains(wiz.timezone)
    blockReason: ready ? "" : qsTr("Choose a time zone.")

    function entered() {
        if (!wiz.timezoneChosen && TimeZones.problem === "") {
            // A suggestion only: the single (or main) zone of the language's country,
            // else the zone of the running session.
            const fromLocale = TimeZones.suggestionForTerritory(Locales.territoryOf(wiz.locale))
            if (fromLocale !== "") wiz.timezone = fromLocale
            else if (TimeZones.systemZone !== "") wiz.timezone = TimeZones.systemZone
        }
        const r = TimeZones.regionOf(wiz.timezone)
        if (r !== "") region = r
        regions.positionAtSelection()
        zones.positionAtSelection()
    }

    Timer { interval: 5000; running: page.visible; repeat: true; onTriggered: page.tick++ }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: TimeZones.problem !== ""
        icon: "clock"
        tone: "warning"
        title: qsTr("The list of time zones is unavailable")
        message: TimeZones.problem
    }

    NxSearchField {
        id: search
        Layout.fillWidth: true
        visible: TimeZones.problem === ""
        placeholderText: qsTr("Search %1 cities, countries and zones").arg(TimeZones.count)
        Keys.onDownPressed: zones.focusList()
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: TimeZones.problem === ""
        spacing: Theme.spaceMd

        PickList {
            id: regions
            Layout.fillWidth: false
            Layout.preferredWidth: 200
            enabled: search.text.trim() === ""
            opacity: enabled ? 1 : Theme.opacityDisabled
            model: TimeZones.regions
            keyRole: "key"
            titleRole: "name"
            trailingRole: "count"
            selectedKey: page.region
            accessibleName: qsTr("Region")
            onPicked: (key) => page.region = key
        }
        PickList {
            id: zones
            model: TimeZones.zones(page.region, search.text)
            keyRole: "id"
            titleRole: "city"
            subtitleRole: "detail"
            trailingRole: "offsetText"
            selectedKey: page.wiz.timezone
            accessibleName: qsTr("Time zone")
            emptyText: qsTr("No time zone matches “%1”.").arg(search.text)
            onPicked: (key) => { page.wiz.timezone = key; page.wiz.timezoneChosen = true }
        }
    }

    NxCard {
        Layout.fillWidth: true
        visible: TimeZones.problem === ""
        implicitHeight: preview.implicitHeight + padding * 2
        RowLayout {
            id: preview
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.spaceMd
            NxIcon { name: "clock"; size: Theme.iconLarge; color: Theme.accent }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: { page.tick; return TimeZones.timeIn(page.wiz.timezone) || qsTr("Time not available for this zone") }
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    readonly property var zone: TimeZones.describe(page.wiz.timezone)
                    text: page.wiz.timezone + (zone.offsetText ? " · " + zone.offsetText : "") + (zone.detail ? " · " + zone.detail : "")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
            Text {
                text: qsTr("Current time there")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }
}
