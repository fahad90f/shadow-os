// SPDX-License-Identifier: GPL-3.0-or-later
//
// The quick-settings tile's texts must stay readable in every state: WCAG AA,
// 4.5:1 for its caption-size subtitle and its title, against the tile on the
// quick-settings card (Theme.bgOverlay), with and without the hover overlay,
// in the dark and the light theme, for every accent while the tile is on,
// and in each tone ("" | "warning" | "danger", the kill switch tile's).
//
// Run in the build chroot with a build directory's org.shadow.ui (scripts/dev.sh
// exec puts its qml/ on QML_IMPORT_PATH and lib/ on LD_LIBRARY_PATH):
//   scripts/dev.sh exec --dir <DIR> -- bash -c 'HOME=$(mktemp -d) QT_QPA_PLATFORM=offscreen \
//     /usr/lib/qt6/bin/qmltestrunner -input /src/desktop/horizon-shell/modules/status/tests/tst_status_tile_contrast.qml'
// or with an installed one (DESTDIR=<stage>), from the top of the repository:
//   QML_IMPORT_PATH=<stage>/usr/lib/x86_64-linux-gnu/qt6/qml LD_LIBRARY_PATH=<stage>/usr/lib/x86_64-linux-gnu \
//   QT_QPA_PLATFORM=offscreen HOME=$(mktemp -d) \
//   /usr/lib/qt6/bin/qmltestrunner -input desktop/horizon-shell/modules/status/tests/tst_status_tile_contrast.qml
// (HOME: the test switches Appearance.dark and the accent, which are saved there.)
import QtQuick
import QtTest
import org.shadow.ui
import "../qml"

TestCase {
    id: testCase
    name: "StatusTileContrast"

    // desktop/themes/tokens/shadow-tokens.json "accent"
    readonly property var accents: ["cyan", "blue", "violet", "emerald", "amber", "rose"]

    Component {
        id: tileComponent
        StatusTile {
            width: 190
            icon: "lock-open"
            title: "Kill switch"
            subtitle: "Off for this boot"
        }
    }

    function lin(c) { return c <= 0.03928 ? c / 12.92 : Math.pow((c + 0.055) / 1.055, 2.4) }
    function luminance(c) { return 0.2126 * lin(c.r) + 0.7152 * lin(c.g) + 0.0722 * lin(c.b) }
    // A colour with alpha over an opaque one.
    function over(fg, bg) {
        const a = fg.a
        return Qt.rgba(fg.r * a + bg.r * (1 - a), fg.g * a + bg.g * (1 - a), fg.b * a + bg.b * (1 - a), 1)
    }
    function ratio(a, b) {
        const la = luminance(a), lb = luminance(b)
        return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05)
    }
    // The Text item showing 'text' (found by its text, whatever its name).
    function textItem(item, text) {
        if (item.text === text && item.font !== undefined && item.color !== undefined)
            return item
        for (let i = 0; i < item.children.length; ++i) {
            const found = textItem(item.children[i], text)
            if (found)
                return found
        }
        return null
    }

    function test_texts_data() {
        const rows = []
        for (const dark of [true, false]) {
            for (const tone of ["", "warning", "danger"]) {
                for (const active of [false, true])
                    rows.push({ tag: (dark ? "dark" : "light") + " tone=" + (tone || "none") + (active ? " on" : " off"),
                                dark: dark, tone: tone, active: active })
            }
        }
        return rows
    }

    function test_texts(data) {
        Appearance.dark = data.dark
        const accentList = data.active ? accents : [Appearance.accent]
        for (const accent of accentList) {
            if (data.active && Appearance.accent !== accent)
                Appearance.accent = accent
            const tile = createTemporaryObject(tileComponent, testCase, { tone: data.tone, active: data.active })
            verify(tile !== null, "StatusTile created")
            const tileColour = over(tile.color, Theme.bgOverlay)
            const hovered = over(Theme.hoverOverlay, tileColour)
            for (const label of [tile.subtitle, tile.title]) {
                const text = textItem(tile, label)
                verify(text !== null, "a Text shows '" + label + "'")
                for (const [where, bg] of [["tile", tileColour], ["hovered tile", hovered]]) {
                    const r = ratio(text.color, bg)
                    verify(r >= 4.5, "'%1' (%2) on the %3 (%4), accent %5: %6:1, below 4.5:1"
                           .arg(label).arg(text.color).arg(where).arg(bg).arg(accent).arg(r.toFixed(2)))
                }
            }
        }
    }
}
