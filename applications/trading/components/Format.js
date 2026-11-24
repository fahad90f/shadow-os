// SPDX-License-Identifier: GPL-3.0-or-later
// Number and time formatting shared by every page.
.pragma library

function usable(v) {
    return v !== undefined && v !== null && isFinite(v)
}

// A price with as many decimals as its size needs.
function price(v) {
    if (!usable(v) || v === 0)
        return "—"
    var a = Math.abs(v)
    return Number(v).toLocaleString(Qt.locale(), "f", a >= 100 ? 2 : a >= 1 ? 4 : 6)
}

function money(v) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", 2) : "—"
}

function sign(v) {
    return v > 0 ? "+" : v < 0 ? "−" : ""
}

function signedMoney(v) {
    return usable(v) ? sign(v) + Math.abs(v).toLocaleString(Qt.locale(), "f", 2) : "—"
}

function signedPct(v, decimals) {
    if (!usable(v))
        return "—"
    return sign(v) + Math.abs(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 2 : decimals) + " %"
}

function pct(v, decimals) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 1 : decimals) + " %" : "—"
}

function quantity(v) {
    if (!usable(v))
        return "—"
    var a = Math.abs(v)
    return Number(v).toLocaleString(Qt.locale(), "f", a >= 100 ? 2 : a >= 1 ? 4 : 6)
}

function number(v, decimals) {
    return usable(v) ? Number(v).toLocaleString(Qt.locale(), "f", decimals === undefined ? 2 : decimals) : "—"
}

function clock(ms) {
    return usable(ms) && ms > 0 ? Qt.formatTime(new Date(ms), "HH:mm:ss") : "—"
}

function stamp(ms) {
    return usable(ms) && ms > 0 ? Qt.formatDateTime(new Date(ms), "d MMM HH:mm") : "—"
}

// The base currency of "BTC/USDT".
function base(name) {
    var i = name.indexOf("/")
    return i > 0 ? name.substring(0, i) : name
}
