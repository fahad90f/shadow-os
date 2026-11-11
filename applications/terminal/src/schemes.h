// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

// Terminal colour schemes.
//
// SHADOW Terminal ships two schemes of its own, generated from the design
// tokens at build time and installed as QTermWidget ".colorscheme" files:
// "SHADOW Obsidian" (dark) and "SHADOW Daylight" (light). They are registered
// with QTermWidget next to the schemes that library ships itself.
namespace Schemes {

// Register the SHADOW scheme directory with QTermWidget. Call once, before the
// first terminal is created.
void registerSchemes();

// Where the SHADOW schemes were found: the data directory, or the copy
// embedded in the binary (a ":/" path) when that directory is missing.
QString directory();

// Names of all schemes QTermWidget knows, SHADOW schemes first.
QStringList available();

// Display name of a scheme ("SHADOW Obsidian" for ShadowObsidian).
QString label(const QString &name);

// The SHADOW scheme for an appearance mode.
QString automatic(bool dark);

// The scheme a profile value stands for: "auto" (or an unknown name) follows
// the appearance mode, anything else is used as it is.
QString resolve(const QString &profileValue, bool dark);

// Data behind "--dump schemes".
QJsonObject toJson();

} // namespace Schemes
