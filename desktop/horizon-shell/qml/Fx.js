// SPDX-License-Identifier: GPL-3.0-or-later
.pragma library

// Arithmetic of the reveal choreography (see OverlaySurface.qml).
//
// Every overlay has one linear clock, "reveal", that runs from 0 to 1 while
// it opens and back to 0 while it closes. Each part of the surface takes a
// window [a, b] of that clock; nothing has a delay or a timer of its own.

function clamp01(v) { return v < 0 ? 0 : v > 1 ? 1 : v }

// How far the clock p is through the window [a, b]: 0 before, 1 after.
function lin(p, a, b) { return b > a ? clamp01((p - a) / (b - a)) : (p >= b ? 1 : 0) }

// The same, decelerating (out-cubic): for arrivals.
function ph(p, a, b) {
    const u = 1 - lin(p, a, b)
    return 1 - u * u * u
}

// When a row starts: a beam crosses the surface in the window
// [beamA, beamB] at constant speed, and the row lies at 'fraction' (0..1) of
// the beam's way. The row starts when the beam reaches it.
function itemStart(beamA, beamB, fraction) { return beamA + (beamB - beamA) * clamp01(fraction) }

// The window of a row: starts with the beam, takes 'span', and is moved
// forward where it would end after 'latest'.
function rowPhase(p, beamA, beamB, fraction, span, latest) {
    const start = Math.min(itemStart(beamA, beamB, fraction), latest - span)
    return ph(p, start, start + span)
}
