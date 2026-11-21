// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.ui

// "Persona" design tokens (a QML singleton, registered in CMakeLists.txt):
// palette, type, space, motion. Colours follow Appearance.dark through
// Theme.dark; every duration collapses to 0 with reduce motion.
QtObject {
    readonly property bool dark: Theme.dark
    readonly property bool reduceMotion: Theme.reduceMotion
    readonly property bool software: Appearance.softwareRendering

    // ---- stage --------------------------------------------------------------
    readonly property color bg: dark ? "#06080C" : "#ECEEF4"
    readonly property color surface: dark ? "#0E131C" : "#FFFFFF"
    readonly property color surfaceHi: dark ? "#151C29" : "#ECEEF5"
    readonly property color sunken: dark ? "#080B11" : "#E6E9F0"
    readonly property color line: dark ? "#1B2231" : "#E0E3EB"
    readonly property color lineStrong: dark ? "#2B3548" : "#C5CBD8"
    // cards, prompt bar, sheets (slightly lifted from the stage)
    readonly property color card: dark ? "#0C111A" : "#FFFFFF"
    readonly property color sheet: dark ? "#0D121C" : "#FFFFFF"
    readonly property color codeBg: dark ? "#0A0F17" : "#F0F1F6"
    readonly property color codeInk: dark ? "#DCE4F2" : "#1B2236"

    // ---- ink ----------------------------------------------------------------
    readonly property color ink: dark ? "#ECEFF6" : "#0D1222"
    readonly property color inkSoft: dark ? "#A9B2C5" : "#454D64"
    readonly property color inkMute: dark ? "#757F95" : "#626A82"
    readonly property color inkFaint: dark ? "#3F4759" : "#A3A9B8"

    // ---- brand light (the persona is drawn with it) -------------------------
    readonly property color cyan: dark ? "#22D8F5" : "#067C99"
    readonly property color blue: dark ? "#3D8BFF" : "#1D5FE0"
    readonly property color violet: dark ? "#9B6BFF" : "#6D3FE0"
    readonly property color rose: dark ? "#FF8FB8" : "#C2306A"      // her voice: the gradient, continued
    readonly property color you: dark ? "#5BE2F7" : "#05687F"        // speaker label "You"
    readonly property color her: dark ? "#BBA2FF" : "#5E33D1"        // speaker label "Shadow"
    readonly property color codeSpan: dark ? "#CDB8FF" : "#5A32C4"   // inline code
    // the sentence she is speaking (a soft violet mark behind the text)
    readonly property color spokenMark: dark ? "#B57BFF" : "#B65CF0"
    readonly property real spokenMarkAlpha: dark ? 0.26 : 0.18

    readonly property color starCore: dark ? "#F4FCFF" : "#1F2766"
    readonly property color starLineA: dark ? "#3FDDF6" : "#2F63D8"
    readonly property color starLineB: dark ? "#A07CFF" : "#7445DD"
    readonly property color mouthA: dark ? "#B994FF" : "#6D3FE0"
    readonly property color mouthB: dark ? "#FF95BE" : "#CB3A74"

    // ---- semantic -----------------------------------------------------------
    readonly property color ok: dark ? "#3BE3A6" : "#08744C"
    readonly property color caution: dark ? "#FFC85A" : "#8A5608"
    readonly property color alarm: dark ? "#FF6E88" : "#C8294A"

    // ---- type ---------------------------------------------------------------
    readonly property string sans: "Inter Variable"
    readonly property string mono: "JetBrains Mono"
    readonly property int display: 34
    readonly property int title: 24
    readonly property int lead: 17
    readonly property int body: 15
    readonly property int ui: 13
    readonly property int small: 12
    readonly property int label: 11
    readonly property int code: 13
    readonly property real labelTracking: 1.6

    // ---- shape --------------------------------------------------------------
    readonly property int rChip: 6
    readonly property int rCard: 10
    readonly property int rSheet: 16

    // ---- motion -------------------------------------------------------------
    readonly property int tQuick: reduceMotion ? 0 : 140
    readonly property int tMood: reduceMotion ? 0 : 420
    readonly property int tDock: reduceMotion ? 0 : 520
    readonly property int tSheet: reduceMotion ? 0 : 260

    function alpha(c: color, a: real): color {
        return Qt.rgba(c.r, c.g, c.b, a)
    }
    function mix(a: color, b: color, t: real): color {
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t)
    }
    // Text on a chip tinted with its own tone (alpha ~0.13 fill): pulled
    // towards the ink so small labels keep at least 4.5:1 in both themes.
    function chipText(tone: color): color {
        return mix(tone, ink, dark ? 0.2 : 0.3)
    }
    // "#rrggbb" (opaque) for rich-text CSS
    function hex(c: color): string {
        const h = (v) => ("0" + Math.round(v * 255).toString(16)).slice(-2)
        return "#" + h(c.r) + h(c.g) + h(c.b)
    }
    // "rgba(r, g, b, a)" for rich-text CSS (Qt reads the alpha as 0..255)
    function css(c: color, a: real): string {
        return "rgba(" + Math.round(c.r * 255) + ", " + Math.round(c.g * 255) + ", " + Math.round(c.b * 255)
               + ", " + Math.round(a * 255) + ")"
    }
    // Brand gradient across the face: 0 = cyan side, 1 = violet side.
    function brandAt(t: real): color {
        return t < 0.5 ? mix(starLineA, blue, t * 2) : mix(blue, starLineB, (t - 0.5) * 2)
    }
}
