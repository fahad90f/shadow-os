// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// Where the API key is typed or pasted. Save writes it to a file only this
// user can read (AssistantConfig.saveApiKey) and tries it at once with one
// tiny request; the field never shows a saved key again, only its last four
// characters.
ColumnLayout {
    id: kf
    required property StageModel stage
    readonly property AssistantConfig cfg: stage.cfg
    readonly property ModelCatalog catalog: stage.agent.catalog
    // "ok" | "bad" | "busy" | "" — the outcome line under the field
    property string resultKind: ""
    property string resultText: ""
    // a key from the environment is used before the saved one
    readonly property bool overridden: cfg.apiKeySaved && cfg.apiKeySource !== "Settings" && cfg.apiKeyConfigured
    signal saved()
    // put the keyboard in the field when the page shows (the welcome page)
    property bool autoFocus: false
    onVisibleChanged: if (visible && autoFocus) Qt.callLater(field.forceActiveFocus)
    Component.onCompleted: if (visible && autoFocus) Qt.callLater(field.forceActiveFocus)

    spacing: 8

    function save() {
        const key = field.text.trim()
        if (key === "")
            return
        const problem = cfg.saveApiKey(key)
        if (problem !== "") {
            resultKind = "bad"
            resultText = problem
            return
        }
        field.text = ""
        reveal.checked = false
        saved()
        test()
    }
    function test() {
        resultKind = "busy"
        resultText = qsTr("Trying the key…")
        catalog.checkKey()
    }
    function remove() {
        cfg.removeSavedApiKey()
        resultKind = ""
        resultText = ""
        stage.whisper(qsTr("Saved key removed"), "info")
    }

    // Env-gated test hook (SHADOW_ASSISTANT_TEST_KEY, empty in normal use):
    // the text is put in the field that is on screen and Save is pressed.
    property bool testDone: false
    Timer {
        interval: 600
        running: kf.visible && kf.stage.agent.testKey !== "" && !kf.testDone
        onTriggered: {
            kf.testDone = true
            field.text = kf.stage.agent.testKey
            kf.save()
        }
    }

    Connections {
        target: kf.catalog
        function onKeyChecked(ok: bool, message: string) {
            kf.resultKind = ok ? "ok" : "bad"
            kf.resultText = message
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            radius: 19
            color: Pe.sunken
            border.width: field.activeFocus ? 2 : 1
            border.color: field.activeFocus ? Pe.alpha(Pe.violet, 0.7) : Pe.lineStrong
            PeIcon {
                id: keyIcon
                x: 13
                anchors.verticalCenter: parent.verticalCenter
                name: "key"
                size: 15
                color: Pe.inkMute
            }
            TextField {
                id: field
                anchors.fill: parent
                anchors.leftMargin: 36
                anchors.rightMargin: 40
                background: null
                echoMode: reveal.checked ? TextInput.Normal : TextInput.Password
                passwordMaskDelay: 0
                inputMethodHints: Qt.ImhSensitiveData | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                placeholderText: kf.cfg.apiKeySaved ? qsTr("A key is saved — paste a new one to replace it")
                                                    : qsTr("Paste your key here (nvapi-…)")
                placeholderTextColor: Pe.inkMute
                color: Pe.ink
                font.family: Pe.mono
                font.pixelSize: Pe.small
                verticalAlignment: TextInput.AlignVCenter
                selectByMouse: true
                Accessible.name: qsTr("API key")
                Accessible.passwordEdit: true
                onAccepted: kf.save()
                onTextEdited: {
                    if (kf.resultKind === "bad") {
                        kf.resultKind = ""
                        kf.resultText = ""
                    }
                }
            }
            PeIconButton {
                id: reveal
                checkable: true
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                visible: field.text !== ""
                iconName: checked ? "eye-off" : "eye"
                tip: checked ? qsTr("Hide what you typed") : qsTr("Show what you typed")
            }
        }
        PeButton {
            kind: "primary"
            iconName: "check"
            text: qsTr("Save")
            tip: qsTr("Save the key on this computer and try it")
            enabled: field.text.trim() !== ""
            onClicked: kf.save()
        }
    }

    // what happened
    RowLayout {
        Layout.fillWidth: true
        visible: kf.resultText !== ""
        spacing: 8
        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 5
            implicitWidth: 7; implicitHeight: 7; radius: 3.5
            color: kf.resultKind === "ok" ? Pe.ok : kf.resultKind === "bad" ? Pe.alarm : Pe.her
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: kf.resultText
            font.family: Pe.sans; font.pixelSize: Pe.small; lineHeight: 1.25
            color: kf.resultKind === "bad" ? Pe.chipText(Pe.alarm) : kf.resultKind === "ok" ? Pe.chipText(Pe.ok) : Pe.inkSoft
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
        }
    }
    Text {
        textFormat: Text.PlainText
        Layout.fillWidth: true
        visible: kf.overridden
        wrapMode: Text.Wrap
        text: qsTr("A key from the %1 is used instead of the saved one.").arg(kf.cfg.apiKeySource)
        font.family: Pe.sans; font.pixelSize: Pe.small
        color: Pe.chipText(Pe.caution)
    }

    Flow {
        Layout.fillWidth: true
        spacing: 4
        PeButton {
            compact: true; kind: "ghost"; iconName: "lab"
            text: kf.catalog.checkingKey ? qsTr("Trying…") : qsTr("Test the key")
            tip: qsTr("Send one tiny request with the key in use")
            visible: kf.cfg.apiKeyConfigured && !kf.cfg.localMode
            enabled: !kf.catalog.checkingKey
            onClicked: kf.test()
        }
        PeButton {
            compact: true; kind: "ghost"; iconName: "trash"
            text: qsTr("Remove saved key")
            visible: kf.cfg.apiKeySaved
            onClicked: kf.remove()
        }
        PeButton {
            compact: true; kind: "ghost"; iconName: "copy"
            text: qsTr("Where to get a key")
            tip: kf.cfg.keyHelpUrl
            onClicked: {
                kf.stage.agent.copyToClipboard(kf.cfg.keyHelpUrl)
                kf.stage.whisper(qsTr("Link copied: %1").arg(kf.cfg.keyHelpUrl), "info")
            }
        }
    }
}
