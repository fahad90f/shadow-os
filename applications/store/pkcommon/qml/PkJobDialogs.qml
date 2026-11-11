// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// The questions a NexusPk::Job can raise, one dialog each. A dialog opens
// when the job's promptKind says so; accepting calls job.answer(true), every
// other way of closing it (Cancel, Escape) calls job.answer(false) — which
// ends the job without changing the system.
//
//   confirm    the changes a simulation found (always shown for removals)
//   untrusted  packages that cannot be authenticated — danger, opt-in only
//   eula       a licence agreement PackageKit requires
//   key        a repository signing key that is not trusted yet
//   media      another installation medium is needed
Item {
    id: root

    // NexusPk::Job (may be null)
    property QtObject job: null

    readonly property string kind: job ? job.promptKind : ""
    readonly property var prompt: job ? job.prompt : ({})

    function names(list) {
        var out = []
        for (var i = 0; list && i < list.length; i++) out.push(list[i].name)
        return out.join(", ")
    }
    function lines(list) {
        var out = []
        for (var i = 0; list && i < list.length; i++) out.push(list[i].name + "  " + list[i].version + "  (" + list[i].arch + ")")
        return out.join("\n")
    }

    function sync() {
        var dialogs = { "confirm": confirmDialog, "untrusted": untrustedDialog, "eula": eulaDialog,
                        "key": keyDialog, "media": mediaDialog }
        for (var k in dialogs) {
            var d = dialogs[k]
            if (k === kind && !d.visible) d.open()
            else if (k !== kind && d.visible) d.close()
        }
    }

    onKindChanged: sync()
    onPromptChanged: sync()

    // Common frame: fixed width, scrolling body, explicit buttons, Escape = decline.
    component PromptDialog: Dialog {
        id: dlg
        default property alias body: bodyColumn.data
        // Inline components do not see the ids of the file around them, so the job is handed in.
        property QtObject job: null
        property string acceptText: qsTr("Continue")
        property string rejectText: qsTr("Cancel")
        property bool danger: false
        property bool acceptEnabled: true

        width: Math.min(580, (Overlay.overlay ? Overlay.overlay.width : 640) - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        onAccepted: if (job && job.promptKind !== "") job.answer(true)
        onRejected: if (job && job.promptKind !== "") job.answer(false)

        contentItem: ColumnLayout {
            spacing: Theme.spaceLg
            Flickable {
                id: flick
                Layout.fillWidth: true
                implicitHeight: Math.min(bodyColumn.implicitHeight, 380)
                contentHeight: bodyColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                ScrollBar.vertical: ScrollBar { }
                ColumnLayout {
                    id: bodyColumn
                    width: flick.width
                    spacing: Theme.spaceMd
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: dlg.rejectText
                    onClicked: dlg.reject()
                }
                NxButton {
                    text: dlg.acceptText
                    kind: dlg.danger ? "danger" : "primary"
                    enabled: dlg.acceptEnabled
                    onClicked: dlg.accept()
                }
            }
        }
    }

    component BodyText: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
    }

    component Heading: Text {
        Layout.fillWidth: true
        color: Theme.textPrimary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        font.weight: Theme.weightSemibold
        wrapMode: Text.WordWrap
        textFormat: Text.PlainText
    }

    component MonoBlock: Rectangle {
        property alias text: mono.text
        Layout.fillWidth: true
        implicitHeight: mono.implicitHeight + Theme.spaceMd * 2
        radius: Theme.radiusSm
        color: Theme.bgInput
        border.width: Theme.borderWidth
        border.color: Theme.border
        TextEdit {
            id: mono
            anchors.fill: parent
            anchors.margins: Theme.spaceMd
            textFormat: TextEdit.PlainText
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            color: Theme.textPrimary
            selectionColor: Theme.accentFillStrong
            selectedTextColor: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    // ---- confirm --------------------------------------------------------------
    PromptDialog {
        id: confirmDialog
        job: root.job
        readonly property var c: root.kind === "confirm" ? root.prompt : ({})
        readonly property int removeCount: c.removeCount || 0
        title: root.job && root.job.kind === "remove" ? qsTr("Remove these packages?")
             : removeCount > 0 ? qsTr("This will also remove packages") : qsTr("Confirm the changes")
        danger: removeCount > 0
        acceptText: root.job && root.job.kind === "remove" ? qsTr("Remove")
                  : removeCount > 0 ? qsTr("Continue and remove") : qsTr("Continue")

        BodyText {
            text: root.job ? qsTr("%1 — PackageKit calculated the following changes. Nothing has been changed yet.").arg(root.job.title) : ""
        }
        Heading {
            visible: confirmDialog.removeCount > 0
            color: Theme.danger
            text: qsTr("Will be removed (%1)").arg(confirmDialog.removeCount)
        }
        MonoBlock { visible: confirmDialog.removeCount > 0; text: root.lines(confirmDialog.c.remove) }
        Heading { visible: (confirmDialog.c.installCount || 0) > 0; text: qsTr("Will be installed (%1)").arg(confirmDialog.c.installCount || 0) }
        BodyText { visible: (confirmDialog.c.installCount || 0) > 0; text: root.names(confirmDialog.c.install) }
        Heading { visible: (confirmDialog.c.updateCount || 0) > 0; text: qsTr("Will be updated (%1)").arg(confirmDialog.c.updateCount || 0) }
        BodyText { visible: (confirmDialog.c.updateCount || 0) > 0; text: root.names(confirmDialog.c.update) }
        Heading { visible: (confirmDialog.c.downgradeCount || 0) > 0; text: qsTr("Will be downgraded (%1)").arg(confirmDialog.c.downgradeCount || 0) }
        BodyText { visible: (confirmDialog.c.downgradeCount || 0) > 0; text: root.names(confirmDialog.c.downgrade) }
        Heading { visible: (confirmDialog.c.reinstallCount || 0) > 0; text: qsTr("Will be reinstalled (%1)").arg(confirmDialog.c.reinstallCount || 0) }
        BodyText { visible: (confirmDialog.c.reinstallCount || 0) > 0; text: root.names(confirmDialog.c.reinstall) }
        Heading { visible: (confirmDialog.c.obsoleteCount || 0) > 0; text: qsTr("Will be replaced as obsolete (%1)").arg(confirmDialog.c.obsoleteCount || 0) }
        BodyText { visible: (confirmDialog.c.obsoleteCount || 0) > 0; text: root.names(confirmDialog.c.obsolete) }
        BodyText {
            visible: (confirmDialog.c.downloadSize || 0) > 0
            text: qsTr("Download size: %1").arg(Theme.formatBytes(confirmDialog.c.downloadSize || 0))
        }
        BodyText {
            visible: confirmDialog.removeCount === 0 && (confirmDialog.c.installCount || 0) === 0
                     && (confirmDialog.c.updateCount || 0) === 0 && (confirmDialog.c.downgradeCount || 0) === 0
                     && (confirmDialog.c.reinstallCount || 0) === 0
            text: qsTr("PackageKit reported no package to install, update or remove for this operation.")
        }
    }

    // ---- untrusted --------------------------------------------------------------
    PromptDialog {
        id: untrustedDialog
        job: root.job
        readonly property var p: root.kind === "untrusted" ? root.prompt : ({})
        title: qsTr("These packages cannot be authenticated")
        danger: true
        acceptText: qsTr("Install unverified packages")
        acceptEnabled: riskBox.checked
        onOpened: riskBox.checked = false

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxIcon { Layout.alignment: Qt.AlignTop; name: "shield-alert"; size: Theme.iconXL; color: Theme.danger }
            BodyText {
                color: Theme.textPrimary
                text: qsTr("PackageKit could not verify that the packages below come from a signed, trusted software source. "
                           + "They may have been altered on the way to this computer. Installing them gives their contents "
                           + "full administrator access to the system.")
            }
        }
        Heading {
            visible: (untrustedDialog.p.packages || []).length > 0
            color: Theme.danger
            text: qsTr("Not authenticated (%1)").arg((untrustedDialog.p.packages || []).length)
        }
        MonoBlock { visible: (untrustedDialog.p.packages || []).length > 0; text: root.lines(untrustedDialog.p.packages) }
        Heading { visible: (untrustedDialog.p.details || "") !== ""; text: qsTr("PackageKit says (%1)").arg(untrustedDialog.p.errorKey || qsTr("no error code")) }
        MonoBlock { visible: (untrustedDialog.p.details || "") !== ""; text: untrustedDialog.p.details || "" }
        BodyText {
            text: qsTr("The safe choice is Cancel: nothing is installed. If you continue, PackageKit asks for administrator "
                       + "authorisation to install untrusted packages.")
        }
        CheckBox {
            id: riskBox
            Layout.fillWidth: true
            text: qsTr("I understand the risk and want to install packages that cannot be verified")
        }
    }

    // ---- licence agreement ----------------------------------------------------------
    PromptDialog {
        id: eulaDialog
        job: root.job
        readonly property var p: root.kind === "eula" ? root.prompt : ({})
        title: qsTr("Licence agreement")
        acceptText: qsTr("Accept")
        rejectText: qsTr("Decline")

        BodyText {
            text: qsTr("“%1” from %2 can only be installed if you accept this licence agreement.")
                .arg(eulaDialog.p.packageName || "").arg(eulaDialog.p.vendor || qsTr("an unnamed vendor"))
        }
        MonoBlock { text: eulaDialog.p.licenseAgreement || "" }
        BodyText {
            visible: (eulaDialog.p.remaining || 0) > 0
            text: qsTr("%1 more agreement(s) follow.").arg(eulaDialog.p.remaining || 0)
        }
    }

    // ---- signing key ----------------------------------------------------------------
    PromptDialog {
        id: keyDialog
        job: root.job
        readonly property var p: root.kind === "key" ? root.prompt : ({})
        title: qsTr("Trust this signing key?")
        danger: true
        acceptText: qsTr("Trust key")
        rejectText: qsTr("Do not trust")

        BodyText {
            text: qsTr("The software source “%1” is signed with a key this system does not trust yet. Trust it only if "
                       + "the fingerprint matches what the provider of the source publishes; a trusted key can sign any "
                       + "package you install from it.").arg(keyDialog.p.repository || "")
        }
        NxKeyValue { label: qsTr("Key owner"); value: keyDialog.p.keyUserId || "" }
        NxKeyValue { label: qsTr("Key ID"); value: keyDialog.p.keyId || ""; mono: true }
        NxKeyValue { label: qsTr("Fingerprint"); value: keyDialog.p.keyFingerprint || ""; mono: true }
        NxKeyValue { label: qsTr("Created"); value: keyDialog.p.keyTimestamp || "" }
        NxKeyValue { label: qsTr("Key location"); value: keyDialog.p.keyUrl || ""; mono: true }
        NxKeyValue { label: qsTr("Needed for"); value: keyDialog.p.packageName || "" }
    }

    // ---- media change -----------------------------------------------------------------
    PromptDialog {
        id: mediaDialog
        job: root.job
        readonly property var p: root.kind === "media" ? root.prompt : ({})
        title: qsTr("Another medium is needed")
        acceptText: qsTr("Try again")

        BodyText {
            text: (mediaDialog.p.text || "") !== "" ? mediaDialog.p.text
                : qsTr("PackageKit needs a different installation medium to continue.")
        }
        BodyText {
            visible: (mediaDialog.p.mediaId || "") !== ""
            text: qsTr("Medium: %1 (%2)").arg(mediaDialog.p.mediaId || "").arg(mediaDialog.p.mediaType || "")
        }
        BodyText { text: qsTr("Insert it, then choose “Try again”.") }
    }
}
