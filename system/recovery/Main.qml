// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.recovery

NxWindow {
    id: window
    title: qsTr("SHADOW Recovery")
    width: 980
    height: 860

    Recovery {
        id: recovery
        onActionFinished: (action, ok, message) => {
            if (message !== "")
                window.toaster.show(message, ok ? "success" : "danger")
        }
    }
    Component.onCompleted: recovery.rescan()

    readonly property var target: recovery.current
    readonly property bool hasTarget: recovery.currentId !== "" && target.id === recovery.currentId
    readonly property bool targetRunning: hasTarget && target.running === true
    readonly property bool targetMounted: hasTarget && !!target.mounted_by_recovery
    // Nothing is running — and something could be run (not the read-only test mode).
    readonly property bool idle: !recovery.busy && !recovery.scanning && !recovery.testMode
    // For a console, when SHADOW Terminal is not there.
    readonly property string shellCommand: hasTarget ? "sudo shadow-recovery-engine shell --target " + recovery.currentId + " --exec" : ""

    function describe(installation) {
        const parts = [installation.root.device, Theme.formatBytes(installation.root.size), installation.root.fstype]
        if (installation.root.disk_model)
            parts.push(installation.root.disk_model)
        if (installation.hostname)
            parts.push(qsTr("host name %1").arg(installation.hostname))
        return parts.join("  ·  ")
    }
    function userNames() {
        // people first, root last: the first entry is the dialog's default
        if (!hasTarget || !target.users)
            return []
        return target.users.filter(u => u.uid !== 0).concat(target.users.filter(u => u.uid === 0)).map(u => u.name)
    }

    NxPage {
        anchors.fill: parent
        maxContentWidth: 860
        title: qsTr("SHADOW Recovery")
        subtitle: recovery.liveSession
                  ? qsTr("Repair a Shadow OS installation on this computer's disks. Nothing is changed until you start a repair.")
                  : qsTr("Repair this Shadow OS installation, or another one on this computer's disks. Nothing is changed until you start a repair.")
        actions: [
            NxButton {
                text: qsTr("Scan again")
                iconName: "refresh"
                busy: recovery.scanning
                enabled: !recovery.busy && !recovery.scanning && recovery.engineProblem === ""
                onClicked: recovery.rescan()
            }
        ]

        NxBanner {
            Layout.fillWidth: true
            visible: recovery.testMode
            kind: "warning"
            title: qsTr("Test mode")
            message: qsTr("The list below comes from a recorded scan (--scan-file). The engine is not run and nothing can be repaired.")
        }

        NxEmptyState {
            Layout.fillWidth: true
            Layout.preferredHeight: 260
            visible: recovery.engineProblem !== ""
            icon: "alert-triangle"
            tone: "danger"
            title: qsTr("SHADOW Recovery cannot work here")
            message: recovery.engineProblem
        }

        NxEmptyState {
            Layout.fillWidth: true
            Layout.preferredHeight: 220
            visible: recovery.engineProblem === "" && !recovery.scanned
            icon: "search"
            title: qsTr("Looking for installed systems…")
            message: qsTr("The disks are only read. If you are asked to authenticate, that is for examining them.")
        }

        NxEmptyState {
            Layout.fillWidth: true
            Layout.preferredHeight: 260
            visible: recovery.engineProblem === "" && recovery.scanned && recovery.scanError !== ""
            icon: "alert-triangle"
            tone: "danger"
            title: qsTr("The disks could not be examined")
            message: recovery.scanError
            actionText: qsTr("Try again")
            onActionTriggered: recovery.rescan()
        }

        // ---- 1. which installation ------------------------------------------------
        NxSection {
            visible: recovery.engineProblem === "" && recovery.scanned && recovery.scanError === ""
            title: qsTr("Installed systems")
            description: qsTr("Choose the installation to repair. Encrypted disks have to be unlocked before the system on them can be found.")

            NxRow {
                visible: recovery.installations.length === 0
                icon: "info"
                title: qsTr("No installed system was found")
                subtitle: recovery.lockedVolumes.length > 0
                          ? qsTr("There is an encrypted volume: unlock it to look inside.")
                          : qsTr("No disk of this computer holds a Linux installation that could be read.")
            }
            Repeater {
                model: recovery.installations
                NxRow {
                    id: installationRow
                    required property var modelData
                    readonly property bool selected: modelData.id === recovery.currentId
                    icon: modelData.running ? "laptop" : "harddrive"
                    iconColor: selected ? Theme.accent : Theme.textSecondary
                    title: modelData.name
                    subtitle: window.describe(modelData)
                    NxBadge { anchors.verticalCenter: parent.verticalCenter; visible: installationRow.modelData.running; text: qsTr("This system"); kind: "accent" }
                    NxBadge { anchors.verticalCenter: parent.verticalCenter; visible: installationRow.modelData.encrypted; text: qsTr("Encrypted"); kind: "neutral" }
                    NxBadge { anchors.verticalCenter: parent.verticalCenter; visible: !!installationRow.modelData.mounted_by_recovery; text: qsTr("Mounted"); kind: "success" }
                    NxBadge { anchors.verticalCenter: parent.verticalCenter; visible: !installationRow.modelData.is_shadow; text: qsTr("Not Shadow OS"); kind: "warning" }
                    NxBadge { anchors.verticalCenter: parent.verticalCenter; visible: installationRow.selected; text: qsTr("Selected"); kind: "accent"; dot: false }
                    NxButton {
                        visible: installationRow.modelData.is_shadow && !installationRow.selected
                        text: qsTr("Select")
                        enabled: window.idle
                        onClicked: recovery.select(installationRow.modelData.id)
                    }
                }
            }
            Repeater {
                model: recovery.lockedVolumes
                NxRow {
                    id: lockedRow
                    required property var modelData
                    icon: "disk-lock"
                    title: qsTr("Encrypted volume")
                    subtitle: [modelData.device, Theme.formatBytes(modelData.size), modelData.label || ""].filter(s => s !== "").join("  ·  ")
                    NxButton {
                        text: qsTr("Unlock…")
                        iconName: "lock-open"
                        enabled: window.idle
                        onClicked: { unlockDialog.device = lockedRow.modelData.device; unlockDialog.open() }
                    }
                }
            }
            Repeater {
                model: recovery.inactiveLvm
                NxRow {
                    id: lvmRow
                    required property var modelData
                    icon: "layers"
                    title: modelData.volume_group ? qsTr("Inactive volume group %1").arg(modelData.volume_group) : qsTr("LVM physical volume")
                    subtitle: modelData.volume_group ? modelData.device
                              : qsTr("%1 — its volume group could not be determined (LVM tools missing).").arg(modelData.device)
                    NxButton {
                        visible: !!lvmRow.modelData.volume_group
                        text: qsTr("Activate")
                        enabled: window.idle
                        onClicked: recovery.activateLvm(lvmRow.modelData.volume_group)
                    }
                }
            }
            Repeater {
                model: recovery.unreadable
                NxRow {
                    required property var modelData
                    icon: "alert-triangle"
                    iconColor: Theme.warning
                    title: qsTr("%1 could not be read").arg(modelData.device)
                    subtitle: modelData.problem
                }
            }
        }

        // ---- 2. repairs ------------------------------------------------------------
        NxSection {
            visible: window.hasTarget && recovery.engineProblem === ""
            title: window.hasTarget ? qsTr("Repair %1").arg(window.target.name) : ""
            description: window.targetRunning
                         ? qsTr("This is the system that is running: repairs are applied to it directly.")
                         : qsTr("Repairs work inside the installation, which is mounted for them under a private folder. Each action says what it changes; none starts by itself.")

            NxRow {
                visible: !window.targetRunning
                icon: window.targetMounted ? "folder-open" : "folder"
                title: window.targetMounted ? qsTr("Mounted for repairs") : qsTr("Not mounted")
                subtitle: window.targetMounted ? window.target.mounted_by_recovery
                          : qsTr("It is mounted automatically when a repair needs it.")
                NxButton {
                    text: window.targetMounted ? qsTr("Unmount") : qsTr("Mount")
                    enabled: window.idle
                    onClicked: window.targetMounted ? recovery.unmountCurrent() : recovery.mountCurrent()
                }
            }
            Repeater {
                model: window.hasTarget && window.target.problems ? window.target.problems : []
                NxRow {
                    required property string modelData
                    icon: "alert-triangle"
                    iconColor: Theme.warning
                    title: qsTr("Found while scanning")
                    subtitle: modelData
                }
            }

            ActionRow {
                icon: "harddrive"
                title: qsTr("Check the file systems")
                explanation: qsTr("Looks for damage in the installation's file systems (root, /boot, EFI partition) with e2fsck and fsck.vfat. They are unmounted for the check.")
                changes: qsTr("Nothing. If damage is found, a separate repair is offered.")
                unavailableReason: window.targetRunning ? qsTr("Not possible for the running system: start from the Shadow OS medium and choose Recovery.") : ""
                result: recovery.results.fsck
                running: recovery.busyAction === "fsck"
                NxButton {
                    visible: recovery.results.fsck !== undefined && recovery.results.fsck.errorsFound === true
                    text: qsTr("Repair…")
                    kind: "danger"
                    enabled: window.idle
                    onClicked: fsckConfirm.open()
                }
                NxButton {
                    text: qsTr("Check")
                    busy: recovery.busyAction === "fsck"
                    enabled: window.idle && !window.targetRunning
                    onClicked: recovery.checkFilesystems(false)
                }
            }
            ActionRow {
                icon: "power"
                title: qsTr("Repair the boot loader")
                explanation: qsTr("For a system that no longer starts: rebuilds the initial RAM disk of every installed kernel (update-initramfs -u -k all), re-installs GRUB the way the system was installed (shadow-grub-install) and regenerates the boot menu (update-grub).")
                changes: qsTr("The files in /boot, the boot loader on the EFI partition or in the disk's boot sector, and the firmware's “Shadow OS” boot entry.")
                result: recovery.results.boot
                running: recovery.busyAction === "boot"
                NxButton {
                    text: qsTr("Repair")
                    kind: "primary"
                    busy: recovery.busyAction === "boot"
                    enabled: window.idle
                    onClicked: recovery.repairBoot()
                }
            }
            ActionRow {
                icon: "package"
                title: qsTr("Repair the package system")
                explanation: qsTr("For an update that was interrupted: finishes half-installed packages (dpkg --configure -a) and repairs broken dependencies (apt-get -f install), which may download packages if the installation's network settings work from here.")
                changes: qsTr("Installed packages and their configuration; packages may be installed or removed to make the system consistent.")
                result: recovery.results.packages
                running: recovery.busyAction === "packages"
                NxButton {
                    text: qsTr("Repair")
                    kind: "primary"
                    busy: recovery.busyAction === "packages"
                    enabled: window.idle
                    onClicked: recovery.repairPackages()
                }
            }
            ActionRow {
                icon: "key"
                title: qsTr("Set a new password")
                explanation: qsTr("For a forgotten password: sets a new one for a user account of the installation (chpasswd) and forgets its failed log-in attempts.")
                changes: qsTr("The account's password. Data encrypted with the old password, such as a saved-passwords wallet, stays locked.")
                unavailableReason: window.hasTarget && window.userNames().length === 0 ? qsTr("The installation has no login accounts.") : ""
                result: recovery.results.password
                running: recovery.busyAction === "password"
                NxButton {
                    text: qsTr("Set password…")
                    busy: recovery.busyAction === "password"
                    enabled: window.idle && window.userNames().length > 0
                    onClicked: passwordDialog.open()
                }
            }
            ActionRow {
                icon: "file-text"
                title: qsTr("Save logs to…")
                explanation: qsTr("Collects what helps to find the cause — the installer's log, the journal of the last three starts, package and boot configuration, and what Recovery did — into one archive you can attach to a problem report.")
                changes: qsTr("Nothing on the installation. A new .tar.gz file is written where you say; it can contain host and user names.")
                result: recovery.results.logs
                running: recovery.busyAction === "logs"
                NxButton {
                    text: qsTr("Save logs…")
                    busy: recovery.busyAction === "logs"
                    enabled: window.idle
                    onClicked: { logPath.text = recovery.suggestedLogFile(); logsDialog.open() }
                }
            }
            ActionRow {
                visible: !window.targetRunning
                icon: "terminal"
                title: qsTr("Open a terminal in the installed system")
                explanation: recovery.terminalAvailable
                    ? qsTr("For repairs by hand: a root shell whose / is the installation (chroot).")
                    : qsTr("SHADOW Terminal is not installed here. From a console, the same root shell is started with: %1").arg(window.shellCommand)
                changes: qsTr("Whatever you type there.")
                result: recovery.results.terminal
                running: recovery.busyAction === "terminal"
                NxButton {
                    visible: recovery.terminalAvailable
                    text: qsTr("Open terminal")
                    enabled: window.idle
                    onClicked: recovery.openTerminal()
                }
            }
        }

        // ---- 3. what happened ------------------------------------------------------
        NxSection {
            visible: recovery.engineProblem === "" && (recovery.logText !== "" || window.hasTarget)
            title: qsTr("Output")
            description: recovery.engineLogFile !== ""
                         ? qsTr("Every command and its output. The engine also keeps it in %1.").arg(recovery.engineLogFile)
                         : qsTr("Every command a repair runs appears here with its output.")
            headerActions: [
                NxIconButton { iconName: "copy"; tooltip: qsTr("Copy the output"); enabled: recovery.logText !== ""
                               onClicked: { recovery.copyLog(); window.toaster.show(qsTr("Output copied to the clipboard"), "success") } },
                NxIconButton { iconName: "trash"; tooltip: qsTr("Clear the output"); enabled: recovery.logText !== "" && !recovery.busy
                               onClicked: recovery.clearLog() }
            ]

            Item {
                Layout.fillWidth: true
                implicitHeight: 240

                Text {
                    anchors.centerIn: parent
                    visible: recovery.logText === ""
                    text: qsTr("Nothing has been run yet.")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                Flickable {
                    id: flick
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    contentWidth: width
                    contentHeight: log.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar { }
                    property bool follow: true
                    onMovementEnded: follow = atYEnd
                    onContentHeightChanged: if (follow) contentY = Math.max(0, contentHeight - height)

                    TextEdit {
                        id: log
                        width: flick.width - Theme.spaceMd
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        text: recovery.logText
                        color: Theme.textSecondary
                        selectionColor: Theme.accentFillStrong
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        activeFocusOnTab: true
                        Accessible.name: qsTr("Output of the recovery engine")
                    }
                }
            }
        }

        // ---- 4. done ---------------------------------------------------------------
        NxSection {
            visible: recovery.engineProblem === ""
            title: qsTr("When you are done")
            ActionRow {
                icon: "refresh"
                title: qsTr("Restart the computer")
                explanation: qsTr("Unmounts the installation, closes encrypted volumes that were unlocked here and restarts. Remove the Shadow OS medium when the screen goes dark to start the repaired system.")
                changes: ""
                unavailableReason: recovery.canRestart ? "" : recovery.restartProblem
                result: recovery.results.restart
                running: recovery.busyAction === "restart"
                NxButton {
                    text: qsTr("Restart…")
                    enabled: window.idle && recovery.canRestart
                    onClicked: restartConfirm.open()
                }
            }
        }
    }

    // ---- dialogs -------------------------------------------------------------------
    SecretDialog {
        id: unlockDialog
        property string device: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("Unlock the encrypted volume")
        message: qsTr("Enter the passphrase of %1. It is passed to cryptsetup and not stored.").arg(device)
        fieldLabel: qsTr("Passphrase")
        confirmText: qsTr("Unlock")
        onSecretAccepted: secret => recovery.unlock(device, secret)
    }
    SecretDialog {
        id: passwordDialog
        anchors.centerIn: parent
        modal: true
        title: qsTr("Set a new password")
        message: qsTr("Choose the account and type its new password twice. The old password is replaced; this cannot be undone.")
        fieldLabel: qsTr("New password")
        confirmText: qsTr("Set password")
        repeat: true
        destructive: true
        choices: window.userNames()
        validate: (first, second) => recovery.passwordProblem(first, second)
        onSecretAccepted: secret => recovery.resetPassword(choices[choiceIndex], secret)
    }
    NxConfirmDialog {
        id: fsckConfirm
        anchors.centerIn: parent
        modal: true
        title: qsTr("Repair the file systems?")
        message: qsTr("The check found damage. The repair (e2fsck -y, fsck.vfat -a) rewrites file-system structures; files that cannot be recovered are moved to lost+found or removed. If the data matters and there is no backup, copy the disk first.")
        confirmText: qsTr("Repair")
        destructive: true
        onConfirmed: recovery.checkFilesystems(true)
    }
    NxConfirmDialog {
        id: restartConfirm
        anchors.centerIn: parent
        modal: true
        title: qsTr("Restart the computer?")
        message: qsTr("Unsaved work in other applications is lost.")
        confirmText: qsTr("Restart")
        onConfirmed: recovery.restart()
    }
    Dialog {
        id: logsDialog
        anchors.centerIn: parent
        modal: true
        width: Math.min(560, window.width - Theme.spaceXxl)
        title: qsTr("Save logs to…")
        closePolicy: Popup.CloseOnEscape
        footer: null
        onOpened: logPath.forceActiveFocus()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Text {
                Layout.fillWidth: true
                text: qsTr("File to create (a .tar.gz archive). An existing file is never overwritten.")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            TextField {
                id: logPath
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("Path of the archive")
                onAccepted: if (saveButton.enabled) saveButton.clicked()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: logsDialog.close() }
                NxButton {
                    id: saveButton
                    text: qsTr("Save")
                    kind: "primary"
                    enabled: logPath.text.trim() !== ""
                    onClicked: { logsDialog.close(); recovery.saveLogs(logPath.text) }
                }
            }
        }
    }
}
