// SPDX-License-Identifier: GPL-3.0-or-later
// Small formatting helpers shared by the pages.

function has(v) { return v !== undefined && v !== null }

// A tri-state from the report: true / false / null ("could not be determined").
function tri(v, yes, no, unknown) {
    return v === true ? yes : v === false ? no : unknown
}

function list(v) { return Array.isArray(v) ? v : [] }

function filterRows(rows, text, roles) {
    const needle = (text || "").trim().toLowerCase()
    if (needle === "") return rows
    return rows.filter(function (row) {
        for (let i = 0; i < roles.length; i++) {
            const v = row[roles[i]]
            if (v !== undefined && v !== null && String(v).toLowerCase().indexOf(needle) >= 0) return true
        }
        return false
    })
}

function sortRows(rows, role, ascending) {
    if (!role) return rows
    const numeric = /^-?\d+(\.\d+)?$/
    const out = rows.slice()
    out.sort(function (a, b) {
        let x = a[role], y = b[role]
        const xn = x === undefined || x === null, yn = y === undefined || y === null
        if (xn || yn) return xn && yn ? 0 : xn ? 1 : -1          // unknown values last
        if (numeric.test(String(x)) && numeric.test(String(y))) { x = Number(x); y = Number(y) }
        else { x = String(x).toLowerCase(); y = String(y).toLowerCase() }
        const r = x < y ? -1 : x > y ? 1 : 0
        return ascending ? r : -r
    })
    return out
}
