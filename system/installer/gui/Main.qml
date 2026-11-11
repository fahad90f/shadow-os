// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

NxWindow {
    id: window

    title: Installer.testMode ? qsTr("TEST MODE: synthetic hardware, cannot install — NEXUS OS Installer")
                              : qsTr("NEXUS OS Installer")
    width: 1024
    height: 700
    minimumWidth: 960
    minimumHeight: 620

    readonly property var allSteps: [
        { key: "welcome", title: qsTr("Welcome"), icon: "language" },
        { key: "keyboard", title: qsTr("Keyboard"), icon: "keyboard" },
        { key: "check", title: qsTr("System check"), icon: "shield-check" },
        { key: "timezone", title: qsTr("Time zone"), icon: "clock" },
        { key: "disk", title: qsTr("Disk"), icon: "harddrive" },
        { key: "user", title: qsTr("User"), icon: "user" },
        { key: "software", title: qsTr("Software"), icon: "package" },
        { key: "summary", title: qsTr("Summary"), icon: "list" },
        { key: "progress", title: qsTr("Installation"), icon: "download" },
        { key: "result", title: qsTr("Finish"), icon: "flag" }
    ]
    // The software step exists only when tool profiles are present on this system.
    readonly property var steps: allSteps.filter(s => s.key !== "software" || Installer.profiles.length > 0)
    readonly property var pages: ({ welcome: welcomePage, keyboard: keyboardPage, check: checkPage, timezone: timezonePage,
                                    disk: diskPage, user: userPage, software: softwarePage, summary: summaryPage,
                                    progress: progressPage, result: resultPage })
    property string currentKey: "welcome"
    readonly property int currentIndex: steps.findIndex(s => s.key === currentKey)
    readonly property var currentPage: pages[currentKey]
    readonly property bool installing: Installer.installState === "running"
    readonly property bool inWizard: currentKey !== "progress" && currentKey !== "result"
    property string pendingStart: ""
    property string lastInstallState: "idle"

    function go(key) {
        currentKey = key
        pages[key].focusHeading()
        pages[key].entered()
    }
    function back() {
        if (inWizard && currentIndex > 0)
            go(steps[currentIndex - 1].key)
    }
    function next() {
        if (!inWizard || !currentPage.ready)
            return
        if (currentKey === "summary") {
            wiz.planChangedNotice = ""
            if (Installer.startInstall(Installer.plan.hash))
                go("progress")
            return
        }
        wiz.planChangedNotice = ""
        go(steps[currentIndex + 1].key)
    }

    WizardState { id: wiz }

    Component.onCompleted: {
        if (Installer.testMode)
            wiz.applyPreset(Installer.preset)
        const start = Installer.startStep
        if (start === "" || start === "welcome" || start === "keyboard" || steps.findIndex(s => s.key === start) < 0) {
            go(start === "keyboard" ? "keyboard" : "welcome")
        } else {
            // --step (test mode): later steps need the hardware report first.
            pendingStart = start
            Installer.runProbe(false)
        }
    }

    Connections {
        target: Installer
        function onProbeChanged() {
            if (window.pendingStart === "" || Installer.probeState === "running")
                return
            const start = window.pendingStart
            if (Installer.probeState !== "ready") { window.pendingStart = ""; window.go("check"); return }
            if (start === "progress" || start === "result") {
                if (window.currentKey !== "summary") window.go("summary")
            } else {
                window.pendingStart = ""
                window.go(start)
            }
        }
        function onPlanChanged() {
            if ((window.pendingStart === "progress" || window.pendingStart === "result") && Installer.planState !== "running"
                    && Installer.planState !== "idle") {
                window.pendingStart = ""
                if (Installer.planState === "ready" && Installer.startInstall(Installer.plan.hash))
                    window.go("progress")
            }
        }
        function onInstallChanged() {
            const state = Installer.installState
            if (state === window.lastInstallState)
                return
            window.lastInstallState = state
            if (state === "succeeded" || state === "failed") {
                window.go("result")
            } else if (state === "plan-changed") {
                // Engine exit status 3: what was confirmed is no longer what would be done.
                wiz.planChangedNotice = Installer.errorMessage + (Installer.errorHint !== "" ? " " + Installer.errorHint : "")
                        + " " + qsTr("Nothing was written. A new plan is shown below; read it and confirm again.")
                Installer.resetInstall()
                window.go("summary")
            }
        }
    }

    onClosing: (close) => {
        if (window.installing) {
            close.accepted = false
            busyDialog.open()
        }
    }

    Dialog {
        id: busyDialog
        title: qsTr("The installation is running")
        modal: true
        anchors.centerIn: parent
        width: Math.min(460, window.width - Theme.spaceXxl)
        footer: null
        closePolicy: Popup.CloseOnEscape
        contentItem: ColumnLayout {
            spacing: Theme.spaceLg
            Text {
                Layout.fillWidth: true
                text: qsTr("The installer stays open until the engine has finished. Closing it now would interrupt the engine "
                         + "and leave the target disk in an unusable state.")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            NxButton {
                Layout.alignment: Qt.AlignRight
                kind: "primary"
                text: qsTr("Keep installing")
                onClicked: busyDialog.close()
            }
        }
    }

    Shortcut { sequence: "Alt+Left"; enabled: backButton.visible && backButton.enabled; onActivated: window.back() }
    Shortcut { sequence: "Alt+Right"; enabled: nextButton.visible && nextButton.enabled; onActivated: window.next() }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ------------------------------------------------------------ sidebar
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 228
            color: Theme.bgSurface

            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.borderSubtle }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceXxs

                RowLayout {
                    Layout.fillWidth: true
                    Layout.margins: Theme.spaceSm
                    Layout.bottomMargin: Theme.spaceMd
                    spacing: Theme.spaceMd
                    NxIcon { name: "nexus"; size: Theme.iconLarge; color: Theme.accent }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Text {
                            text: qsTr("NEXUS OS")
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBodyLarge
                            font.weight: Theme.weightSemibold
                        }
                        Text {
                            text: qsTr("Installer")
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                Repeater {
                    model: window.steps
                    Rectangle {
                        id: stepItem
                        required property var modelData
                        required property int index
                        readonly property bool current: index === window.currentIndex
                        readonly property bool done: index < window.currentIndex
                        // Earlier steps can be revisited while the wizard is still collecting choices.
                        readonly property bool reachable: done && window.inWizard

                        Layout.fillWidth: true
                        implicitHeight: 38
                        radius: Theme.radiusSm
                        color: current ? Theme.accentFill : hover.hovered && reachable ? Theme.hoverOverlay : "transparent"
                        border.width: activeFocus ? Theme.focusRing : 0
                        border.color: Theme.accent
                        activeFocusOnTab: reachable
                        Accessible.role: Accessible.Button
                        Accessible.name: qsTr("Step %1: %2").arg(index + 1).arg(modelData.title)
                                         + (current ? qsTr(" (current)") : done ? qsTr(" (done)") : "")
                        Keys.onReturnPressed: if (reachable) window.go(modelData.key)
                        Keys.onSpacePressed: if (reachable) window.go(modelData.key)

                        HoverHandler { id: hover; cursorShape: stepItem.reachable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                        TapHandler { enabled: stepItem.reachable; onTapped: window.go(stepItem.modelData.key) }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceSm
                            anchors.rightMargin: Theme.spaceSm
                            spacing: Theme.spaceMd
                            Rectangle {
                                implicitWidth: 24
                                implicitHeight: 24
                                radius: 12
                                color: stepItem.current ? Theme.accent : stepItem.done ? Theme.alpha(Theme.success, 0.16) : "transparent"
                                border.width: stepItem.current || stepItem.done ? 0 : Theme.borderWidth
                                border.color: Theme.border
                                Text {
                                    anchors.centerIn: parent
                                    visible: !stepItem.done
                                    text: stepItem.index + 1
                                    color: stepItem.current ? Theme.textOnAccent : Theme.textMuted
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: Theme.weightSemibold
                                }
                                NxIcon {
                                    anchors.centerIn: parent
                                    visible: stepItem.done
                                    name: "check"
                                    size: 14
                                    color: Theme.success
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: stepItem.modelData.title
                                color: stepItem.current ? Theme.textPrimary : stepItem.done ? Theme.textSecondary : Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: stepItem.current ? Theme.weightSemibold : Theme.weightRegular
                                elide: Text.ElideRight
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Text {
                    Layout.fillWidth: true
                    Layout.margins: Theme.spaceSm
                    wrapMode: Text.WordWrap
                    color: Installer.testMode ? Theme.warning : Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeCaption
                    text: Installer.testMode ? qsTr("TEST MODE\nSynthetic hardware. Nothing can be installed.")
                        : Installer.liveSession ? qsTr("Live session")
                        : qsTr("Not a live session")
                }
            }
        }

        // ------------------------------------------------------------ content
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                visible: Installer.testMode
                implicitHeight: testText.implicitHeight + Theme.spaceSm * 2
                color: Theme.alpha(Theme.warning, 0.14)
                Text {
                    id: testText
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceXl; anchors.rightMargin: Theme.spaceXl
                    text: qsTr("TEST MODE — the hardware shown is synthetic (%1). This run cannot install anything.").arg(Installer.probeFile)
                    color: Theme.warning
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideMiddle
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                WelcomePage { id: welcomePage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "welcome" }
                KeyboardPage { id: keyboardPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "keyboard" }
                CheckPage { id: checkPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "check" }
                TimeZonePage { id: timezonePage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "timezone" }
                DiskPage { id: diskPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "disk" }
                UserPage { id: userPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "user" }
                SoftwarePage { id: softwarePage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "software" }
                SummaryPage { id: summaryPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "summary" }
                ProgressPage { id: progressPage; anchors.fill: parent; wiz: wiz; visible: window.currentKey === "progress" }
                ResultPage {
                    id: resultPage
                    anchors.fill: parent
                    wiz: wiz
                    visible: window.currentKey === "result"
                    onBackToSummary: { Installer.resetInstall(); window.lastInstallState = "idle"; window.go("summary") }
                }
            }

            // ---------------------------------------------------------- footer
            Rectangle {
                Layout.fillWidth: true
                visible: window.inWizard
                implicitHeight: footer.implicitHeight + Theme.spaceMd * 2
                color: Theme.bgSurface
                Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.borderSubtle }

                RowLayout {
                    id: footer
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceXl; anchors.rightMargin: Theme.spaceXl
                    spacing: Theme.spaceMd

                    NxButton {
                        id: backButton
                        text: qsTr("Back")
                        iconName: "arrow-left"
                        visible: window.currentIndex > 0
                        onClicked: window.back()
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignRight
                        text: window.currentPage.ready ? "" : window.currentPage.blockReason
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideLeft
                        maximumLineCount: 2
                        wrapMode: Text.WordWrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                    NxButton {
                        id: nextButton
                        readonly property bool finalStep: window.currentKey === "summary"
                        text: !finalStep ? qsTr("Next") : Installer.simulation ? qsTr("Run simulation") : qsTr("Install")
                        kind: finalStep && summaryPage.destructive && !Installer.simulation ? "danger" : "primary"
                        iconName: finalStep ? "" : "arrow-right"
                        enabled: window.currentPage.ready
                        onClicked: window.next()
                    }
                }
            }
        }
    }
}
