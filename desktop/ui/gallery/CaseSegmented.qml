// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Test page: the ways applications use NxSegmented.currentIndex.
//   1 unbound                — the selector keeps its own state
//   2 bound to state that the handler changes at once
//   3 bound to a back-end that applies the change later (or refuses it)
//   4 unbound, handler assigns currentIndex itself (older call sites)
//   5 bound, handler re-creates the binding (older work-around)
NxWindow {
    id: window
    title: "NexusUI Gallery — segmented"
    width: 900
    height: 640

    property int mode: 0                // "application state" for 2 and 5
    property int profile: 1             // "back-end state" for 3
    property int requestedProfile: -1
    property bool backendRefuses: false
    property int activations: 0
    Timer {
        id: backend
        interval: 600
        onTriggered: if (!window.backendRefuses) window.profile = window.requestedProfile
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceLg

        NxSection {
            title: "NxSegmented and currentIndex"
            description: "A tap never destroys a binding on currentIndex; without a binding the selector keeps its own state."
            NxRow {
                title: "1 · Not bound"
                subtitle: "currentIndex = " + unbound.currentIndex
                NxSegmented { id: unbound; model: ["Day", "Week", "Month"]; onActivated: window.activations++ }
            }
            NxRow {
                title: "2 · Bound to application state"
                subtitle: "state = " + window.mode + ", currentIndex = " + bound.currentIndex
                NxButton { text: "State → 0"; onClicked: window.mode = 0 }
                NxSegmented {
                    id: bound
                    model: ["All", "TCP", "UDP"]
                    currentIndex: window.mode
                    onActivated: (index) => window.mode = index
                }
            }
            NxRow {
                title: "3 · Bound to a back-end that answers later"
                subtitle: "active profile = " + window.profile + (backend.running ? " (switching to " + window.requestedProfile + "…)" : "")
                Switch { id: refuse; text: "Back-end refuses"; onToggled: window.backendRefuses = checked }
                NxSegmented {
                    id: async
                    model: ["Saver", "Balanced", "Performance"]
                    currentIndex: window.profile
                    onActivated: (index) => { window.requestedProfile = index; backend.restart() }
                }
            }
            NxRow {
                title: "4 · Not bound, handler assigns currentIndex"
                subtitle: "currentIndex = " + assigning.currentIndex
                NxSegmented { id: assigning; model: ["GPT", "MBR"]; onActivated: (index) => currentIndex = index }
            }
            NxRow {
                title: "5 · Bound, handler re-creates the binding"
                subtitle: "state = " + window.mode + ", currentIndex = " + rebinding.currentIndex
                NxSegmented {
                    id: rebinding
                    model: ["All", "TCP", "UDP"]
                    currentIndex: window.mode
                    onActivated: (index) => {
                        window.mode = index
                        currentIndex = Qt.binding(() => window.mode)
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }

    CaseChecks {
        name: "segmented"
        steps: [
            () => {
                unbound.select(2)
                compare(unbound.currentIndex, 2, "unbound: a tap selects the segment")
                compare(window.activations, 1, "unbound: activated emitted once")
                unbound.select(2)
                compare(window.activations, 1, "tapping the current segment emits nothing")
                unbound.currentIndex = 0
                unbound.select(1)
                compare(unbound.currentIndex, 1, "unbound: still its own state after the caller assigned a value")
            },
            () => {
                bound.select(1)
                compare(window.mode, 1, "bound: the handler changed the state")
                compare(bound.currentIndex, 1, "bound: selector shows the new state")
                window.mode = 2
                compare(bound.currentIndex, 2, "bound: the binding survived the tap — selector follows the state")
                compare(rebinding.currentIndex, 2, "the other selector bound to the same state follows too")
            },
            () => {
                async.select(2)
                compare(async.currentIndex, 1, "async: selector keeps showing the active profile until the back-end switches")
            },
            () => {}, () => {}, () => {}, () => {}, () => {}, () => {},
            () => {
                compare(window.profile, 2, "async: back-end switched")
                compare(async.currentIndex, 2, "async: selector followed the back-end")
                window.backendRefuses = true
                async.select(0)
            },
            () => {}, () => {}, () => {}, () => {}, () => {}, () => {},
            () => {
                compare(async.currentIndex, 2, "async: a refused change leaves the selector on the real state")
                async.select(0)
                compare(window.requestedProfile, 0, "async: the same segment can be requested again")
                window.backendRefuses = false
            },
            () => {
                assigning.select(1)
                compare(assigning.currentIndex, 1, "assigning handler: selected")
                assigning.select(0)
                compare(assigning.currentIndex, 0, "assigning handler: selected back")
            },
            () => {
                rebinding.select(0)
                compare(window.mode, 0, "re-binding handler: state changed")
                compare(rebinding.currentIndex, 0, "re-binding handler: selector shows it")
                window.mode = 1
                compare(rebinding.currentIndex, 1, "re-binding handler: still bound")
            }
        ]
    }
}
