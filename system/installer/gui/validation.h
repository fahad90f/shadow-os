// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>

// Input rules of the installation engine, mirrored so that the wizard can
// explain a problem while the user types instead of failing at the summary.
//
// Source of truth: system/installer/engine/nexus_installer/answers.py
// (USERNAME_RE, RESERVED_USERS, HOSTNAME_RE, the fullname/password/passphrase
// checks in parse()). The engine validates everything again; if the two ever
// disagree, the engine wins and its message is shown on the summary page.
// testing/check-rules.py compares these functions with the engine's own code.
namespace Validation {

// Each returns an empty string when the value is acceptable, otherwise a
// sentence that says what is wrong.
QString userNameProblem(const QString &name);
QString fullNameProblem(const QString &name);
QString hostNameProblem(const QString &name);
QString passwordProblem(const QString &password);      // user password: non-empty, single line
QString passphraseProblem(const QString &passphrase);  // disk encryption: at least 8 characters, single line

// Whether 'name' is already taken on the running (live) system by a system
// account or a group. The engine creates the account with
// "useradd --user-group", which fails late — after the disk has been written —
// if the name exists in the copied system. The live system's account database
// is the best available picture of the image's.
QString existingAccountProblem(const QString &name);

// Facts about a password — length and which kinds of characters it contains —
// plus plain-language remarks about concrete weaknesses. There is no score:
// nothing here claims to measure how long an attack would take.
//   { length, lower, upper, digits, symbols, remarks: [..] }
QVariantMap passwordFacts(const QString &password, const QStringList &personalWords);

// A user name derived from a full name ("Ada Lovelace" → "ada"); empty when
// the name contains nothing usable (for example non-Latin script).
QString suggestUserName(const QString &fullName);

} // namespace Validation
