// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading

// The model panel: the NVIDIA key, the models of each role, and a connection
// test. Every model here is one of NVIDIA's hosted models.
NxPage {
    id: page

    title: qsTr("Model Panel")
    subtitle: qsTr("Every model on the panel is hosted by NVIDIA and reached with one key. Each reads the same market snapshot, is briefed with the skills of its role, and votes.")
    actions: [
        NxButton {
            text: qsTr("Test all")
            iconName: "plug"
            enabled: trading.key.hasKey
            onClicked: trading.testAll()
            ToolTip.visible: hovered && !trading.key.hasKey
            ToolTip.text: qsTr("Save the NVIDIA key first.")
        }
    ]

    // model id -> last test result {ok, message}
    property var results: ({})
    readonly property var members: trading.analysts.filter(a => a.kind === "model")
    readonly property var roleEntries: trading.analysts.filter(a => a.kind === "role")

    Labels { id: words }

    Connections {
        target: trading
        function onModelTested(model, ok, message) {
            var copy = Object.assign({}, page.results)
            copy[model] = { ok: ok, message: message }
            page.results = copy
        }
    }
    Component.onCompleted: trading.refreshCatalog()

    // ---- the key ------------------------------------------------------------
    NxSection {
        title: qsTr("NVIDIA API key")
        description: qsTr("One key for every model. It is saved in a file only you can read, sent only to NVIDIA, and never shown again. A key saved in Shadow Assistant is used automatically.")
        headerActions: NxBadge {
            text: trading.key.hasKey ? qsTr("Connected") : qsTr("No key")
            kind: trading.key.hasKey ? "success" : "warning"
        }
        NxRow {
            icon: "key"
            title: trading.key.hasKey ? qsTr("Key ending in %1").arg(trading.key.hint || "····") : qsTr("No key yet")
            subtitle: trading.key.hasKey ? qsTr("In use, %1.").arg(words.keySource(trading.key.source))
                : qsTr("Without a key the rule-based analyst decides alone. Get a key at build.nvidia.com (keys start with nvapi-).")
            NxPasswordField {
                id: keyField
                Layout.preferredWidth: 280
                placeholderText: qsTr("Paste an nvapi- key")
                Accessible.name: qsTr("NVIDIA API key")
                onAccepted: saveButton.clicked()
            }
            NxButton {
                id: saveButton
                kind: "primary"
                text: qsTr("Save")
                enabled: keyField.text.trim() !== ""
                onClicked: {
                    var error = trading.saveKey(keyField.text)
                    if (error === "") {
                        keyField.text = ""
                        ApplicationWindow.window.toaster.show(qsTr("Key saved."), "success")
                    } else {
                        ApplicationWindow.window.toaster.show(error, "danger")
                    }
                }
            }
            NxButton {
                visible: trading.key.source === "saved"
                text: qsTr("Remove")
                onClicked: trading.removeKey()
                Accessible.name: qsTr("Remove the saved NVIDIA key")
            }
        }
    }

    // ---- the panel, role by role -------------------------------------------------
    Repeater {
        model: page.roleEntries

        NxSection {
            id: section
            required property var modelData
            readonly property string role: modelData.role

            title: words.role(role)
            description: (role === "analysis" ? qsTr("The analysis team. Each member reads the snapshot from its own seat: %1. With the team review on, they then read each other's calls and make their final ones.")
                    .arg(trading.teamSeats.map(seat => seat.title.toLowerCase()).join(", "))
                : role === "reasoning" ? qsTr("Argues against the obvious trade before accepting it.")
                : role === "vision" ? qsTr("Also gets a picture of the chart and checks it against the numbers.")
                : qsTr("Works the numbers: distances in ATR, where stops and targets fall."))
                + " " + qsTr("Skills: %1.").arg(modelData.skills.map(s => words.skillTitle(s)).join(", "))
            headerActions: NxBadge {
                text: section.modelData.models === 0 ? qsTr("No model")
                    : qsTr("%1 of %2 on").arg(section.modelData.models).arg(section.modelData.total)
                kind: section.modelData.models === 0 ? "warning" : "neutral"
                dot: false
            }

            NxRow {
                visible: page.members.filter(m => m.role === section.role).length === 0
                icon: "info"
                title: qsTr("No model has this role")
                subtitle: qsTr("Add one below.")
            }
            Repeater {
                model: page.members.filter(m => m.role === section.role)
                NxRow {
                    id: row
                    required property var modelData
                    readonly property var result: page.results[modelData.model]
                    icon: words.roleIcon(modelData.role)
                    iconColor: words.stateColor(modelData.state)
                    title: modelData.seatTitle !== "" ? qsTr("%1  ·  %2 seat").arg(modelData.name).arg(modelData.seatTitle)
                        : modelData.name
                    subtitle: modelData.model + (result !== undefined ? "  ·  " + result.message
                        : modelData.detail !== "" ? "  ·  " + modelData.detail : "")
                    NxBadge {
                        text: words.stateText(row.modelData.state)
                        kind: words.stateKind(row.modelData.state)
                    }
                    NxButton {
                        text: qsTr("Test")
                        busy: row.modelData.state === "working"
                        enabled: trading.key.hasKey && row.modelData.enabled && row.modelData.state !== "working"
                            && row.modelData.state !== "unavailable"
                        onClicked: trading.testModel(row.modelData.model)
                        Accessible.name: qsTr("Test %1").arg(row.modelData.name)
                    }
                    Switch {
                        checked: row.modelData.enabled
                        onToggled: trading.setModelEnabled(row.modelData.model, checked)
                        Accessible.name: qsTr("%1 takes part in analyses").arg(row.modelData.name)
                    }
                    NxIconButton {
                        iconName: "trash"
                        tooltip: qsTr("Take %1 off the panel").arg(row.modelData.name)
                        onClicked: trading.removeModel(row.modelData.model)
                    }
                }
            }
            NxRow {
                visible: section.role === "analysis"
                icon: "users"
                title: qsTr("Team review")
                subtitle: qsTr("After the first calls, every member of the team is shown its teammates' calls and reasoning and makes its final call. It may keep its call. One more request per member.")
                Switch {
                    checked: trading.teamReview
                    onToggled: trading.teamReview = checked
                    Accessible.name: qsTr("Team review")
                }
            }
        }
    }

    // ---- add a model ----------------------------------------------------------------
    NxSection {
        title: qsTr("Add a model")
        description: trading.catalogError !== "" ? qsTr("NVIDIA's list could not be read: %1").arg(trading.catalogError)
            : qsTr("Any chat model NVIDIA offers can join the panel. The list is fetched from NVIDIA.")
        NxRow {
            icon: "plus"
            title: qsTr("Model and role")
            subtitle: qsTr("Choose from NVIDIA's list or type an id such as publisher/model.")
            ComboBox {
                id: addBox
                Layout.preferredWidth: 300
                editable: true
                model: trading.catalog
                Accessible.name: qsTr("Model to add")
            }
            ComboBox {
                id: roleBox
                Layout.preferredWidth: 140
                model: [qsTr("Analysis"), qsTr("Reasoning"), qsTr("Vision"), qsTr("Compute")]
                readonly property var keys: ["analysis", "reasoning", "vision", "compute"]
                Accessible.name: qsTr("Role of the model")
            }
            NxButton {
                kind: "primary"
                text: qsTr("Add")
                enabled: addBox.editText.trim() !== ""
                onClicked: {
                    var error = trading.addModel(addBox.editText, roleBox.keys[roleBox.currentIndex])
                    if (error !== "")
                        ApplicationWindow.window.toaster.show(error, "danger")
                    else
                        addBox.editText = ""
                }
            }
            NxIconButton {
                iconName: "refresh"
                tooltip: qsTr("Fetch NVIDIA's list of models again")
                onClicked: trading.refreshCatalog()
            }
        }
        NxRow {
            icon: "undo"
            title: qsTr("Built-in panel")
            subtitle: qsTr("Fourteen models: six on the analysis team, three for reasoning, three for vision and two for compute.")
            NxButton {
                text: qsTr("Restore")
                onClicked: trading.resetPanel()
                Accessible.name: qsTr("Restore the built-in panel")
            }
        }
    }

    NxSection {
        title: qsTr("Skills")
        description: qsTr("Built in. They run on this computer and need no key.")
        Repeater {
            model: trading.analysts.filter(a => a.kind === "skill")
            NxRow {
                required property var modelData
                icon: words.skillIcon(modelData.id)
                title: words.skillTitle(modelData.id)
                subtitle: modelData.detail !== "" ? modelData.detail
                    : modelData.id === "ta" ? qsTr("EMA, RSI, MACD, Bollinger bands, ATR and volume. Also a brief the models read.")
                    : modelData.id === "smc" ? qsTr("Swings, breaks of structure, order blocks, fair value gaps, liquidity sweeps. Also a brief the models read.")
                    : qsTr("Sizes every order and enforces the limits.")
                NxBadge {
                    text: words.stateText(modelData.state)
                    kind: words.stateKind(modelData.state)
                }
            }
        }
    }
}
