// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Effects
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Users")
    subtitle: qsTr("The people who can sign in to this computer. Changing another person's account asks for an administrator password.")

    // Bumped when a change is refused, so switches show the real state again.
    property int revision: 0
    // Makes a binding depend on 'revision' as well as on the value itself.
    function live(revision, value) { return value }
    property string selectedName: ""
    readonly property var selected: {
        const list = users.users
        for (let i = 0; i < list.length; i++) {
            if (list[i].userName === selectedName) return list[i]
        }
        return ({})
    }
    readonly property bool hasSelection: selected.userName !== undefined

    function selectDefault() {
        if (users.user(selectedName).userName !== undefined) return
        const list = users.users
        let name = ""
        for (let i = 0; i < list.length; i++) {
            if (list[i].current) name = list[i].userName
        }
        selectedName = name !== "" ? name : list.length > 0 ? list[0].userName : ""
    }

    function roleText(user) {
        const parts = [user.userName, user.admin ? qsTr("Administrator") : qsTr("Standard user")]
        if (user.current) parts.push(qsTr("this is you"))
        else if (user.loggedIn) parts.push(qsTr("signed in"))
        if (user.autoLogin) parts.push(qsTr("signs in automatically"))
        return parts.join("  ·  ")
    }

    // Suggest a login name from a full name: first word, ASCII lower case.
    function suggestUserName(fullName) {
        const first = fullName.trim().toLowerCase().split(/\s+/)[0] || ""
        return first.normalize("NFD").replace(/[^a-z0-9_-]/g, "").replace(/^[^a-z_]+/, "").substring(0, 32)
    }

    actions: [
        NxButton {
            text: qsTr("Add user…")
            kind: "primary"
            iconName: "plus"
            enabled: users.available && !users.busy
            onClicked: addDialog.open()
        }
    ]

    // Round picture of an account, or its initial when it has none.
    component Avatar: Item {
        id: avatar
        property var user
        property real size: 40
        implicitWidth: size
        implicitHeight: size
        readonly property bool hasPicture: user !== undefined && user.iconFile !== undefined && user.iconFile !== ""

        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: Theme.accentFill
            visible: !avatar.hasPicture || picture.status !== Image.Ready
            Label {
                anchors.centerIn: parent
                text: avatar.user && avatar.user.displayName ? avatar.user.displayName.charAt(0).toUpperCase() : "?"
                color: Theme.accent
                font.pixelSize: avatar.size * 0.42
                font.weight: Theme.weightSemibold
            }
        }
        Image {
            id: picture
            anchors.fill: parent
            visible: false
            source: avatar.hasPicture ? "file://" + avatar.user.iconFile + "?" + users.iconRevision : ""
            sourceSize: Qt.size(256, 256)
            fillMode: Image.PreserveAspectCrop
            cache: false
            asynchronous: true
            layer.enabled: true
        }
        Rectangle {
            id: mask
            anchors.fill: parent
            radius: width / 2
            visible: false
            layer.enabled: true
        }
        MultiEffect {
            anchors.fill: parent
            visible: avatar.hasPicture && picture.status === Image.Ready
            source: picture
            maskEnabled: true
            maskSource: mask
        }
    }

    UsersBackend {
        id: users
        onChanged: page.selectDefault()
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }

    NxEmptyState {
        visible: users.ready && !users.available
        Layout.fillWidth: true
        tone: "warning"
        icon: "users"
        title: qsTr("User accounts cannot be managed right now")
        message: users.statusMessage
        actionText: qsTr("Try again")
        onActionTriggered: users.refresh()
    }

    NxSection {
        visible: users.available
        title: qsTr("Accounts")

        Repeater {
            model: users.users
            Item {
                id: userRow
                required property var modelData
                required property int index
                readonly property bool isSelected: modelData.userName === page.selectedName
                Layout.fillWidth: true
                implicitHeight: Theme.rowHeightLarge
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: modelData.displayName
                Accessible.description: page.roleText(modelData)
                Keys.onReturnPressed: page.selectedName = modelData.userName
                Keys.onSpacePressed: page.selectedName = modelData.userName

                Rectangle {
                    anchors.fill: parent
                    color: userRow.isSelected ? Theme.accentFill
                         : tap.pressed ? Theme.pressedOverlay
                         : hover.hovered || userRow.activeFocus ? Theme.hoverOverlay : "transparent"
                }
                Rectangle {
                    visible: userRow.index > 0
                    anchors { top: parent.top; left: parent.left; right: parent.right; leftMargin: Theme.spaceLg }
                    height: 1
                    color: Theme.borderSubtle
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler { id: tap; onTapped: page.selectedName = userRow.modelData.userName }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceMd

                    Avatar { user: userRow.modelData; size: 36 }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            text: userRow.modelData.displayName
                            font.weight: Theme.weightMedium
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: page.roleText(userRow.modelData)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                    NxBadge {
                        visible: userRow.modelData.locked
                        text: qsTr("Cannot sign in")
                        kind: "warning"
                    }
                    NxBadge {
                        visible: userRow.modelData.admin
                        text: qsTr("Administrator")
                        kind: "accent"
                        dot: false
                    }
                }
            }
        }
        NxRow {
            visible: users.ready && users.users.length === 0
            icon: "info"
            title: qsTr("AccountsService reports no user accounts")
            subtitle: qsTr("Only system accounts exist on this computer.")
        }
    }

    NxSection {
        visible: users.available && page.hasSelection
        title: page.hasSelection ? page.selected.displayName : ""
        description: page.hasSelection && page.selected.locked
                     ? qsTr("This account is locked or has no password yet: nobody can sign in with it until a password is set.") : ""

        NxRow {
            title: qsTr("Picture")
            subtitle: qsTr("Shown on the sign-in screen.")
            Avatar {
                anchors.verticalCenter: parent.verticalCenter
                user: page.selected
                size: 40
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Choose…")
                enabled: !users.busy
                onClicked: pictureDialog.open()
            }
        }
        NxRow {
            icon: "user"
            title: qsTr("Full name")
            value: page.hasSelection && page.selected.realName !== "" ? page.selected.realName : qsTr("Not set")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Change…")
                enabled: !users.busy
                onClicked: nameDialog.open()
            }
        }
        NxRow {
            icon: "terminal"
            title: qsTr("User name")
            subtitle: page.hasSelection ? qsTr("Home folder %1").arg(page.selected.home) : ""
            value: page.hasSelection ? page.selected.userName : ""
            monoValue: true
        }
        NxRow {
            icon: "key"
            title: qsTr("Password")
            subtitle: page.hasSelection && users.needsCurrentPassword(page.selected.userName)
                      ? qsTr("You need your current password to change it.") : ""
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: page.hasSelection && page.selected.locked ? qsTr("Set password…") : qsTr("Change password…")
                enabled: !users.busy
                onClicked: passwordDialog.open()
            }
        }
        NxRow {
            id: adminRow
            readonly property string block: page.hasSelection ? users.demoteBlockReason(page.selected.userName) : ""
            icon: "shield"
            title: qsTr("Administrator")
            subtitle: block !== "" ? block : qsTr("Administrators can install software and change system settings (members of the “sudo” group).")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: !users.busy && (adminRow.block === "" || !checked)
                checked: page.live(page.revision, page.hasSelection && page.selected.admin)
                Accessible.name: qsTr("Administrator")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, page.hasSelection && page.selected.admin))
                    users.setAdministrator(page.selected.userName, wanted)
                }
            }
        }
        NxRow {
            icon: "power"
            title: qsTr("Sign in automatically")
            subtitle: !users.sddmInstalled ? qsTr("Not available: the SDDM sign-in screen is not installed on this computer.")
                    : users.autoLoginUser !== "" && page.hasSelection && users.autoLoginUser !== page.selected.userName
                      ? qsTr("Currently “%1” is signed in automatically; only one account can be.").arg(users.autoLoginUser)
                    : qsTr("Opens this account's session without asking for the password when the computer starts. Anyone who can switch the computer on gets in.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: users.sddmInstalled && !users.busy
                checked: page.live(page.revision, page.hasSelection && page.selected.autoLogin)
                Accessible.name: qsTr("Sign in automatically")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, page.hasSelection && page.selected.autoLogin))
                    users.setAutoLogin(page.selected.userName, wanted)
                }
            }
        }
        NxRow {
            id: removeRow
            readonly property string block: page.hasSelection ? users.removeBlockReason(page.selected.userName) : ""
            icon: "trash"
            iconColor: block === "" ? Theme.danger : Theme.textSecondary
            title: qsTr("Remove this account")
            subtitle: block !== "" ? block : qsTr("The person can no longer sign in. You decide whether their files are kept.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Remove…")
                kind: "danger"
                enabled: !users.busy && removeRow.block === ""
                onClicked: removeDialog.open()
            }
        }
    }

    // ---- dialogs ------------------------------------------------------------------------

    FileDialog {
        id: pictureDialog
        title: qsTr("Choose a picture")
        fileMode: FileDialog.OpenFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.PicturesLocation)
        nameFilters: [qsTr("Pictures (*.png *.jpg *.jpeg *.webp *.bmp *.svg)")]
        onAccepted: users.setAvatar(page.selectedName, selectedFile)
    }

    Dialog {
        id: nameDialog
        title: qsTr("Full name of %1").arg(page.selectedName)
        width: Math.min(440, page.width - Theme.spaceXxl)
        footer: null
        readonly property string problem: users.realNameProblem(nameField.text.trim())
        function submit() {
            if (problem !== "") return
            users.setRealName(page.selectedName, nameField.text.trim())
            close()
        }
        onOpened: {
            nameField.text = page.hasSelection ? page.selected.realName : ""
            nameField.forceActiveFocus()
            nameField.selectAll()
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("Full name")
                Accessible.name: qsTr("Full name")
                onAccepted: nameDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: nameDialog.problem !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: nameDialog.problem
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: nameDialog.close() }
                NxButton {
                    text: qsTr("Save")
                    kind: "primary"
                    enabled: nameDialog.problem === ""
                    onClicked: nameDialog.submit()
                }
            }
        }
    }

    Dialog {
        id: passwordDialog
        title: page.selectedName === users.currentUserName ? qsTr("Change your password") : qsTr("Password for %1").arg(page.selectedName)
        width: Math.min(460, page.width - Theme.spaceXxl)
        footer: null
        readonly property bool needsCurrent: users.needsCurrentPassword(page.selectedName)
        readonly property string problem: needsCurrent && currentField.text === "" ? qsTr("Enter your current password.")
                                        : users.passwordProblem(newField.text, repeatField.text)
        function clear() { currentField.text = ""; newField.text = ""; repeatField.text = "" }
        function submit() {
            if (problem !== "") return
            users.setPassword(page.selectedName, newField.text, currentField.text)
            close()
        }
        onOpened: { clear(); (needsCurrent ? currentField : newField).forceActiveFocus() }
        onClosed: clear()

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                visible: !passwordDialog.needsCurrent
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("Setting another person's password asks for an administrator password. Passwords stored in their keyring stay locked with the old one.")
            }
            NxPasswordField {
                id: currentField
                Layout.fillWidth: true
                visible: passwordDialog.needsCurrent
                placeholderText: qsTr("Current password")
                Accessible.name: qsTr("Current password")
                onAccepted: newField.forceActiveFocus()
            }
            NxPasswordField {
                id: newField
                Layout.fillWidth: true
                placeholderText: qsTr("New password")
                Accessible.name: qsTr("New password")
                onAccepted: repeatField.forceActiveFocus()
            }
            NxPasswordField {
                id: repeatField
                Layout.fillWidth: true
                placeholderText: qsTr("New password again")
                Accessible.name: qsTr("New password again")
                onAccepted: passwordDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: newField.text !== "" && passwordDialog.problem !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: passwordDialog.problem
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: passwordDialog.close() }
                NxButton {
                    text: qsTr("Change password")
                    kind: "primary"
                    enabled: passwordDialog.problem === ""
                    onClicked: passwordDialog.submit()
                }
            }
        }
    }

    Dialog {
        id: addDialog
        title: qsTr("Add a user")
        width: Math.min(480, page.width - Theme.spaceXxl)
        footer: null
        property bool userNameEdited: false
        readonly property string problem: {
            let p = users.realNameProblem(addFullName.text.trim())
            if (p === "") p = users.userNameProblem(addUserName.text)
            if (p === "") p = users.passwordProblem(addPassword.text, addRepeat.text)
            return p
        }
        function clear() {
            addFullName.text = ""; addUserName.text = ""; addPassword.text = ""; addRepeat.text = ""
            addAdmin.checked = false; userNameEdited = false
        }
        function submit() {
            if (problem !== "") return
            users.addUser(addUserName.text, addFullName.text.trim(), addAdmin.checked, addPassword.text)
            close()
        }
        onOpened: { clear(); addFullName.forceActiveFocus() }
        onClosed: clear()

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            TextField {
                id: addFullName
                Layout.fillWidth: true
                placeholderText: qsTr("Full name")
                Accessible.name: qsTr("Full name")
                onTextEdited: if (!addDialog.userNameEdited) addUserName.text = page.suggestUserName(text)
                onAccepted: addUserName.forceActiveFocus()
            }
            TextField {
                id: addUserName
                Layout.fillWidth: true
                placeholderText: qsTr("User name")
                font.family: Theme.fontMono
                maximumLength: 32
                inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                Accessible.name: qsTr("User name")
                onTextEdited: addDialog.userNameEdited = text !== ""
                onAccepted: addPassword.forceActiveFocus()
            }
            NxPasswordField {
                id: addPassword
                Layout.fillWidth: true
                placeholderText: qsTr("Password")
                Accessible.name: qsTr("Password")
                onAccepted: addRepeat.forceActiveFocus()
            }
            NxPasswordField {
                id: addRepeat
                Layout.fillWidth: true
                placeholderText: qsTr("Password again")
                Accessible.name: qsTr("Password again")
                onAccepted: addDialog.submit()
            }
            CheckBox {
                id: addAdmin
                text: qsTr("Administrator (may install software and change system settings)")
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: (addFullName.text !== "" || addUserName.text !== "") ? addDialog.problem : ""
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: addDialog.close() }
                NxButton {
                    text: qsTr("Add user")
                    kind: "primary"
                    enabled: addDialog.problem === ""
                    onClicked: addDialog.submit()
                }
            }
        }
    }

    Dialog {
        id: removeDialog
        title: qsTr("Remove %1?").arg(page.hasSelection ? page.selected.displayName : "")
        width: Math.min(480, page.width - Theme.spaceXxl)
        footer: null
        closePolicy: Popup.CloseOnEscape
        function submit() {
            if (confirmField.text !== page.selectedName) return
            users.removeUser(page.selectedName, deleteFiles.checked)
            close()
        }
        onOpened: { confirmField.text = ""; deleteFiles.checked = false; confirmField.forceActiveFocus() }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("The account “%1” is deleted and can no longer be used to sign in. This cannot be undone.").arg(page.selectedName)
            }
            CheckBox {
                id: deleteFiles
                text: page.hasSelection ? qsTr("Also delete the home folder %1 and the mailbox").arg(page.selected.home) : ""
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: deleteFiles.checked ? Theme.danger : Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: deleteFiles.checked ? qsTr("All files of this person are deleted permanently.")
                                          : qsTr("The files stay in the home folder. An account created later can receive the same numeric ID and would then own them, so move or delete them soon.")
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Type “%1” to confirm:").arg(page.selectedName)
            }
            TextField {
                id: confirmField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("User name to confirm")
                onAccepted: removeDialog.submit()
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: removeDialog.close() }
                NxButton {
                    text: qsTr("Remove account")
                    kind: "danger"
                    enabled: confirmField.text === page.selectedName && page.selectedName !== ""
                    onClicked: removeDialog.submit()
                }
            }
        }
    }
}
