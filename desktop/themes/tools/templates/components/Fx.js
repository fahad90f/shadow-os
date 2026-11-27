// SPDX-License-Identifier: GPL-3.0-or-later
// Small helpers of the decoration: windows of a progress value, and a colour
// between two colours.
.pragma library

// Share of the window a..b of t that has passed, 0..1, at constant speed
// (scan line, growing lines).
function lin(t, a, b) {
    var x = (t - a) / (b - a)
    return x < 0 ? 0 : x > 1 ? 1 : x
}

// The same, slowing down towards the end (out-cubic): everything that arrives.
function ph(t, a, b) {
    var x = 1 - lin(t, a, b)
    return 1 - x * x * x
}

// The colour between a and b, k = 0..1.
function mix(a, b, k) {
    return Qt.rgba(a.r + (b.r - a.r) * k, a.g + (b.g - a.g) * k, a.b + (b.b - a.b) * k, a.a + (b.a - a.a) * k)
}

// Colour c with opacity a.
function alpha(c, a) {
    return Qt.rgba(c.r, c.g, c.b, a)
}
