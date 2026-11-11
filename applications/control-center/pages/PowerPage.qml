// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Power & Battery")
    subtitle: qsTr("Battery state, power profile, screen brightness and what happens when the computer is idle.")

    function minutesText(m) {
        if (m === 0) return qsTr("Never")
        if (m === 1) return qsTr("1 minute")
        if (m < 60 || m % 60 !== 0) return qsTr("%1 minutes").arg(m)
        return m === 60 ? qsTr("1 hour") : qsTr("%1 hours").arg(m / 60)
    }

    function idleChoices(current) {
        const minutes = [0, 1, 2, 3, 5, 10, 15, 20, 30, 45, 60, 90, 120]
        if (minutes.indexOf(current) < 0) minutes.push(current)
        minutes.sort((a, b) => a - b)
        return minutes.map(m => ({ value: m, text: page.minutesText(m) }))
    }

    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) {
            if (items[i].value === value) return i
        }
        return -1
    }

    function profileIndex() {
        for (let i = 0; i < power.profiles.length; i++) {
            if (power.profiles[i].id === power.activeProfile) return i
        }
        return -1
    }

    function batteryTimeText(b) {
        if (b.stateId === "discharging" && b.timeToEmpty > 0)
            return qsTr("%1 remaining").arg(Theme.formatDuration(b.timeToEmpty))
        if (b.stateId === "charging" && b.timeToFull > 0)
            return qsTr("%1 until full").arg(Theme.formatDuration(b.timeToFull))
        return ""
    }

    // One "after N minutes" setting of ~/.config/nexus/power.conf.
    component IdleRow: NxRow {
        id: idleRow
        property var backend
        property string settingKey: ""
        property int minutes: 0
        property bool usable: true
        property var choices: []
        property int revision: 0

        ComboBox {
            width: 170
            enabled: idleRow.usable
            model: idleRow.choices
            textRole: "text"
            valueRole: "value"
            currentIndex: {
                idleRow.revision
                for (let i = 0; i < idleRow.choices.length; i++) {
                    if (idleRow.choices[i].value === idleRow.minutes) return i
                }
                return -1
            }
            Accessible.name: idleRow.title
            onActivated: idleRow.backend.setIdleMinutes(idleRow.settingKey, currentValue)
        }
    }

    // Bumped when the back-end refuses a change, so controls fall back to the real value.
    property int revision: 0

    PowerBackend {
        id: power
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
    }

    NxBanner {
        visible: power.ready && !power.available
        kind: "warning"
        title: qsTr("Battery information is not available")
        message: power.statusMessage
    }

    // ---- battery ---------------------------------------------------------------
    NxCard {
        visible: power.available && power.hasBattery
        Layout.fillWidth: true
        implicitHeight: 124

        RowLayout {
            anchors.fill: parent
            spacing: Theme.spaceXl

            NxGauge {
                Layout.preferredWidth: 92
                Layout.preferredHeight: 92
                value: (power.battery.percentage || 0) / 100
                text: Math.round(power.battery.percentage || 0) + "%"
                color: (power.battery.percentage || 0) <= 10 ? Theme.danger
                     : (power.battery.percentage || 0) <= 20 ? Theme.warning : Theme.success
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                RowLayout {
                    spacing: Theme.spaceSm
                    NxIcon {
                        name: power.battery.icon || "battery"
                        size: Theme.iconLarge
                        color: Theme.textSecondary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: power.battery.stateText || ""
                        font.pixelSize: Theme.fontSizeTitle
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.batteryTimeText(power.battery)
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: {
                        const parts = [power.onBattery ? qsTr("Running on battery") : qsTr("Connected to mains power")]
                        if (power.battery.energyRate > 0)
                            parts.push(qsTr("%1 W").arg(Number(power.battery.energyRate).toFixed(1)))
                        if (power.battery.energyFull > 0)
                            parts.push(qsTr("%1 of %2 Wh").arg(Number(power.battery.energy).toFixed(1)).arg(Number(power.battery.energyFull).toFixed(1)))
                        return parts.join("  ·  ")
                    }
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
        }
    }

    NxSection {
        visible: power.available
        title: qsTr("Batteries")

        NxRow {
            icon: power.onBattery ? "battery" : "plug"
            title: qsTr("Power source")
            subtitle: power.hasBattery ? "" : qsTr("UPower reports no battery in this computer.")
            value: power.onBattery ? qsTr("Battery") : qsTr("Mains power")
        }
        Repeater {
            model: power.batteries
            NxRow {
                id: batteryRow
                required property string name
                required property string nativePath
                required property real percentage
                required property string stateText
                required property real energyFull
                required property real energyFullDesign
                required property real capacity
                required property int chargeCycles
                required property string technology
                required property real voltage

                icon: "battery"
                title: name
                subtitle: {
                    const parts = []
                    if (capacity > 0) parts.push(qsTr("Health %1 %").arg(Math.round(capacity)))
                    if (chargeCycles > 0) parts.push(qsTr("%1 charge cycles").arg(chargeCycles))
                    if (energyFull > 0 && energyFullDesign > 0)
                        parts.push(qsTr("%1 Wh of %2 Wh design capacity").arg(energyFull.toFixed(1)).arg(energyFullDesign.toFixed(1)))
                    if (technology !== "") parts.push(technology)
                    if (voltage > 0) parts.push(qsTr("%1 V").arg(voltage.toFixed(1)))
                    if (parts.length === 0 && nativePath !== "") parts.push(nativePath)
                    return parts.join("  ·  ")
                }
                value: qsTr("%1 %  ·  %2").arg(Math.round(percentage)).arg(stateText)
            }
        }
    }

    NxSection {
        visible: power.available && power.peripheralCount > 0
        title: qsTr("Connected devices")
        description: qsTr("Batteries of wireless and USB devices, as reported by UPower.")
        Repeater {
            model: power.peripherals
            NxRow {
                required property string name
                required property string kind
                required property string deviceIcon
                required property real percentage
                required property bool hasPercentage
                required property string levelText
                required property string stateText
                required property string stateId

                icon: deviceIcon
                title: name
                subtitle: stateId === "unknown" || stateId === "discharging" ? kind : kind + "  ·  " + stateText
                value: hasPercentage ? qsTr("%1 %").arg(Math.round(percentage))
                                     : levelText !== "" ? levelText : qsTr("Unknown")
            }
        }
    }

    // ---- power profile -----------------------------------------------------------
    NxSection {
        title: qsTr("Power profile")

        NxRow {
            icon: !power.profilesAvailable ? "info"
                : power.activeProfile === "performance" ? "zap" : power.activeProfile === "power-saver" ? "leaf" : "gauge"
            title: power.profilesAvailable ? qsTr("Profile") : qsTr("Power profiles are not available")
            subtitle: {
                if (!power.profilesAvailable) return power.profilesStatus
                const index = page.profileIndex()
                return index >= 0 ? power.profiles[index].description : ""
            }
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                visible: power.profilesAvailable
                model: power.profiles.map(p => p.label)
                currentIndex: (page.revision, page.profileIndex())
                onActivated: (index) => {
                    power.setActiveProfile(power.profiles[index].id)
                    // NxSegmented selects the tapped segment itself; show the profile that is
                    // really active instead (it follows as soon as the daemon has switched).
                    currentIndex = Qt.binding(() => (page.revision, page.profileIndex()))
                }
            }
        }
        NxRow {
            visible: power.profilesAvailable && power.performanceDegraded !== ""
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("Performance is limited")
            subtitle: power.performanceDegradedText
        }
        Repeater {
            model: power.profilesAvailable ? power.profileHolds : []
            NxRow {
                required property var modelData
                icon: "lock"
                title: qsTr("%1 is holding the “%2” profile").arg(modelData.application !== "" ? modelData.application : qsTr("An application")).arg(modelData.profile)
                subtitle: modelData.reason
            }
        }
    }

    // ---- brightness -----------------------------------------------------------------
    NxSection {
        visible: power.brightnessAvailable
        title: qsTr("Screen brightness")
        NxRow {
            icon: "brightness"
            title: qsTr("Built-in display")
            subtitle: qsTr("Backlight “%1”").arg(power.brightnessDevice)
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: Math.round(brightnessSlider.value) + " %"
                color: Theme.textSecondary
                font.family: Theme.fontMono
            }
            Slider {
                id: brightnessSlider
                anchors.verticalCenter: parent.verticalCenter
                width: 260
                from: 1
                to: 100
                stepSize: 1
                Accessible.name: qsTr("Screen brightness")
                onMoved: power.setBrightness(Math.round(value))
                Binding {
                    target: brightnessSlider
                    property: "value"
                    value: power.brightnessPercent
                    when: !brightnessSlider.pressed
                    restoreMode: Binding.RestoreNone
                }
            }
        }
    }

    // ---- idle behaviour ----------------------------------------------------------------
    NxSection {
        title: power.hasBattery || !power.available ? qsTr("When idle on mains power") : qsTr("When idle")
        description: qsTr("Followed by the NEXUS desktop shell. Stored in %1.").arg(power.settingsFile)

        IdleRow {
            icon: "monitor"
            title: qsTr("Turn off the screen after")
            backend: power
            revision: page.revision
            settingKey: "IdleScreenOffMinutes"
            minutes: power.idleScreenOffMinutes
            choices: page.idleChoices(power.idleScreenOffMinutes)
        }
        IdleRow {
            icon: "lock"
            title: qsTr("Lock the session after")
            backend: power
            revision: page.revision
            settingKey: "IdleLockMinutes"
            minutes: power.idleLockMinutes
            choices: page.idleChoices(power.idleLockMinutes)
        }
        IdleRow {
            icon: "moon"
            title: qsTr("Suspend after")
            subtitle: power.suspendUsable || !power.logindAvailable ? "" : qsTr("Suspend is not available on this computer.")
            backend: power
            revision: page.revision
            settingKey: "IdleSuspendMinutes"
            minutes: power.idleSuspendMinutes
            usable: power.suspendUsable || !power.logindAvailable
            choices: page.idleChoices(power.idleSuspendMinutes)
        }
    }

    NxSection {
        visible: power.hasBattery || !power.available
        title: qsTr("When idle on battery")

        IdleRow {
            icon: "monitor"
            title: qsTr("Turn off the screen after")
            backend: power
            revision: page.revision
            settingKey: "IdleScreenOffMinutesOnBattery"
            minutes: power.idleScreenOffMinutesOnBattery
            choices: page.idleChoices(power.idleScreenOffMinutesOnBattery)
        }
        IdleRow {
            icon: "lock"
            title: qsTr("Lock the session after")
            backend: power
            revision: page.revision
            settingKey: "IdleLockMinutesOnBattery"
            minutes: power.idleLockMinutesOnBattery
            choices: page.idleChoices(power.idleLockMinutesOnBattery)
        }
        IdleRow {
            icon: "moon"
            title: qsTr("Suspend after")
            subtitle: power.suspendUsable || !power.logindAvailable ? "" : qsTr("Suspend is not available on this computer.")
            backend: power
            revision: page.revision
            settingKey: "IdleSuspendMinutesOnBattery"
            minutes: power.idleSuspendMinutesOnBattery
            usable: power.suspendUsable || !power.logindAvailable
            choices: page.idleChoices(power.idleSuspendMinutesOnBattery)
        }
    }

    // ---- sleep states and lid (information) -----------------------------------------------
    NxSection {
        title: qsTr("Sleep and lid")
        description: power.logindAvailable ? qsTr("Reported by systemd-logind. The lid switch is configured in logind.conf.")
                                           : qsTr("systemd-logind is not reachable, so sleep support cannot be determined.")

        NxRow {
            icon: "moon"
            title: qsTr("Suspend")
            value: power.capabilityText(power.canSuspend)
        }
        NxRow {
            icon: "harddrive"
            title: qsTr("Hibernate")
            value: power.capabilityText(power.canHibernate)
        }
        NxRow {
            visible: power.lidPresent || !power.available
            icon: "laptop"
            title: qsTr("When the lid is closed")
            subtitle: {
                const source = power.lidActionSource === "logind" ? qsTr("Effective setting of systemd-logind")
                             : power.lidActionSource === "logind.conf" ? qsTr("Read from logind.conf")
                             : qsTr("systemd default; nothing is set in logind.conf")
                return qsTr("%1  ·  on external power: %2  ·  docked: %3")
                    .arg(source).arg(power.actionText(power.lidActionExternalPower)).arg(power.actionText(power.lidActionDocked))
            }
            value: power.actionText(power.lidAction)
        }
    }
}
