// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Scripted checks of a gallery test page (nexus-ui-gallery --case X --selftest).
//
//   CaseChecks {
//       name: "rows"
//       steps: [
//           () => verify(a.isFirst, "first row has no divider"),
//           () => { model.move(0, 2, 1) },          // a later step sees the result
//           () => verify(...)
//       ]
//   }
//
// Steps run one per 'interval', so bindings, layouts and animations settle
// between them. Every verify() prints a line; the first failure makes the
// process exit with status 4 after the last step, which the smoke test
// reports as a failure. Without --selftest nothing runs.
QtObject {
    id: checks
    objectName: "caseChecks"        // main.cpp looks the object up to see whether the checks finished

    property string name: ""
    property var steps: []
    property int interval: 120
    property int failures: 0
    property int passed: 0
    property bool done: false
    readonly property bool active: typeof gallerySelfTest !== "undefined" && gallerySelfTest === true

    function verify(condition, what) {
        if (condition) {
            passed++
            console.log("CASE-PASS " + name + ": " + what)
        } else {
            failures++
            console.error("NEXUS-CASE-FAILED " + name + ": " + what)
        }
    }
    function compare(actual, expected, what) {
        verify(actual === expected, what + " (got " + actual + ", expected " + expected + ")")
    }

    property int _next: 0
    property Timer _timer: Timer {
        interval: checks.interval
        repeat: true
        running: checks.active && !checks.done
        onTriggered: {
            if (checks._next < checks.steps.length) {
                try {
                    checks.steps[checks._next]()
                } catch (e) {
                    checks.failures++
                    console.error("NEXUS-CASE-FAILED " + checks.name + ": step " + checks._next + " threw " + e)
                }
                checks._next++
                return
            }
            checks.done = true
            if (checks.failures > 0) {
                console.error("NEXUS-CASE-FAILED " + checks.name + ": " + checks.failures + " check(s) failed")
                Qt.exit(4)
            } else {
                console.log("NEXUS-CASE-OK " + checks.name + " (" + checks.passed + " checks)")
            }
        }
    }
}
