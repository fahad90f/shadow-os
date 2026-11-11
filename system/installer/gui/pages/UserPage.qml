// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.installer

StepPage {
    id: page
    required property WizardState wiz

    readonly property string fullNameProblem: Installer.fullNameProblem(wiz.fullName)
    readonly property string userNameProblem: Installer.userNameProblem(wiz.userName)
    readonly property string hostNameProblem: Installer.hostNameProblem(wiz.hostName)

    title: qsTr("User")
    subtitle: qsTr("Your account on the installed system. It gets administrator rights through sudo; the root account stays locked.")
    ready: fullNameProblem === "" && userNameProblem === "" && hostNameProblem === "" && Installer.passwordSet
    blockReason: fullNameProblem !== "" ? fullNameProblem
               : wiz.userName === "" ? qsTr("Enter a user name.")
               : userNameProblem !== "" ? userNameProblem
               : !Installer.passwordSet ? qsTr("Enter the password twice.")
               : hostNameProblem

    function entered() { if (wiz.fullName === "" && wiz.userName === "") fullName.forceActiveFocus() }

    component FieldLabel: Text {
        Layout.fillWidth: true
        color: Theme.textSecondary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
    }
    component FieldNote: Text {
        property bool bad: false
        Layout.fillWidth: true
        visible: text !== ""
        wrapMode: Text.WordWrap
        color: bad ? Theme.danger : Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
    }

    ScrollColumn {
        NxCard {
            Layout.fillWidth: true
            implicitHeight: who.implicitHeight + padding * 2
            ColumnLayout {
                id: who
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: Theme.spaceSm

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    uniformCellWidths: true
                    columnSpacing: Theme.spaceMd
                    rowSpacing: Theme.spaceXs
                    FieldLabel { text: qsTr("Full name (optional)") }
                    FieldLabel { text: qsTr("User name") }
                    TextField {
                        id: fullName
                        Layout.fillWidth: true
                        Layout.preferredWidth: 10
                        text: page.wiz.fullName
                        Accessible.name: qsTr("Full name")
                        maximumLength: 200
                        onTextEdited: {
                            page.wiz.fullName = text
                            if (!page.wiz.userNameEdited)
                                page.wiz.userName = Installer.suggestUserName(text)
                        }
                    }
                    TextField {
                        id: userName
                        Layout.fillWidth: true
                        Layout.preferredWidth: 10
                        text: page.wiz.userName
                        font.family: Theme.fontMono
                        Accessible.name: qsTr("User name")
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                        onTextEdited: { page.wiz.userName = text; page.wiz.userNameEdited = text !== "" }
                    }
                    FieldNote { text: page.fullNameProblem; bad: true; visible: true }
                    FieldNote {
                        visible: true
                        bad: page.wiz.userName !== "" && page.userNameProblem !== ""
                        text: page.wiz.userName === "" ? qsTr("Lower-case letters, digits, “-” and “_”; starts with a letter.")
                            : page.userNameProblem !== "" ? page.userNameProblem
                            : qsTr("Home folder: /home/%1").arg(page.wiz.userName)
                    }
                }

                SecretFields {
                    id: password
                    kind: "password"
                    label: qsTr("Password")
                    confirmLabel: qsTr("Repeat password")
                    personalWords: [page.wiz.userName, page.wiz.hostName].concat(page.wiz.fullName.split(" "))
                    held: Installer.passwordSet
                    onCommitted: (secret) => Installer.setPassword(secret)
                }
                FieldNote {
                    text: qsTr("The engine accepts any password that is not empty. The remarks above point out concrete weaknesses; they are not a strength rating.")
                }
            }
        }

        NxSection {
            NxRow {
                icon: "laptop"
                title: qsTr("Computer name")
                subtitle: page.hostNameProblem !== "" ? page.hostNameProblem
                        : qsTr("The name of this computer on the network: lower-case letters, digits and “-”.")
                TextField {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 260
                    text: page.wiz.hostName
                    font.family: Theme.fontMono
                    Accessible.name: qsTr("Computer name")
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    onTextEdited: page.wiz.hostName = text
                }
            }
            NxRow {
                icon: "user"
                title: qsTr("Log in automatically")
                subtitle: page.wiz.encrypt && page.wiz.mode !== "manual"
                          ? qsTr("The desktop starts without asking for the password. The encryption passphrase is still asked at every start.")
                          : qsTr("The desktop starts without asking for the password. Anyone who can switch this computer on gets into your account.")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: page.wiz.autologin
                    Accessible.name: qsTr("Log in automatically")
                    onToggled: page.wiz.autologin = checked
                }
            }
        }
    }
}
